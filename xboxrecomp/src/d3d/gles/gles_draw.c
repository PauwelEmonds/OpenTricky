/*
 * OpenGL ES 3 renderer -- draws, render states, shaders.
 *
 * Three kinds of draw reach the renderer:
 *  - d3d8_nv2a_draw: vertices the translator transformed on the CPU
 *    (ProgVertex: screen xyz + rhw, two colours, fog, four texture
 *    coordinates), shaded by the pixel shader generated from the title's
 *    register combiners (nv2a_psh.c, GLSL in this build);
 *  - DrawPrimitiveUP / DrawIndexedPrimitiveUP with an FVF: the translator's
 *    2D path (pre-transformed quads with one texture), shaded by a
 *    fixed-function shader that follows d3d8_shaders.c;
 *  - the present and the video frames: a textured full-screen triangle.
 *
 * Vertex, index and constant data are streamed through ring buffers mapped
 * unsynchronised (each draw writes past the last; a wrap orphans the buffer),
 * the GL counterpart of the D3D11 renderer's NO_OVERWRITE / DISCARD rings.
 */
#include "gles_internal.h"
#include "../../nv2a/nv2a_psh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef GL_DEPTH_CLAMP_EXT
#define GL_DEPTH_CLAMP_EXT 0x864F
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_CLAMP_TO_BORDER_EXT
#define GL_CLAMP_TO_BORDER_EXT 0x812D
#endif
#ifndef GL_MIRROR_CLAMP_TO_EDGE_EXT
#define GL_MIRROR_CLAMP_TO_EDGE_EXT 0x8743
#endif

int  g_gles_wclip_on;
long g_gles_wclip[4];

static int s_has_depth_clamp, s_has_aniso, s_has_border, s_has_mirror_once;
static float s_max_aniso = 1.0f;
static int s_host_aniso;
static GLint s_ubo_align = 256;

/* ======================================================================== */
/* streaming buffers                                                         */
/* ======================================================================== */

typedef struct {
    GLenum   target;
    GLuint   buf;
    GLsizeiptr size, off;
} Stream;

static Stream s_vtx = { GL_ARRAY_BUFFER, 0, 32 * 1024 * 1024, 0 };
static Stream s_idx = { GL_ELEMENT_ARRAY_BUFFER, 0, 8 * 1024 * 1024, 0 };
static Stream s_ubo = { GL_UNIFORM_BUFFER, 0, 4 * 1024 * 1024, 0 };

/* Copy `n` bytes into the stream; returns the offset, -1 on failure. The
 * buffer stays bound to its target. */
static GLintptr stream_put(Stream *s, const void *data, GLsizeiptr n, GLsizeiptr align)
{
    GLintptr off;
    void *p;
    GLbitfield fl = GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT;
    if (!s->buf) {
        glGenBuffers(1, &s->buf);
        glBindBuffer(s->target, s->buf);
        glBufferData(s->target, s->size, NULL, GL_STREAM_DRAW);
    } else {
        glBindBuffer(s->target, s->buf);
    }
    if (n > s->size) return -1;
    off = (s->off + align - 1) / align * align;
    if (off + n > s->size) {
        glBufferData(s->target, s->size, NULL, GL_STREAM_DRAW);   /* orphan */
        off = 0;
    }
    p = glMapBufferRange(s->target, off, n, fl);
    if (!p) return -1;
    memcpy(p, data, (size_t)n);
    glUnmapBuffer(s->target);
    s->off = off + n;
    return off;
}

/* Reserve room for `total` bytes so several uploads of one draw never wrap
 * between them (the wrap orphans the buffer the earlier ones live in). */
static void stream_reserve(Stream *s, GLsizeiptr total)
{
    if (s->buf && s->off + total > s->size) s->off = s->size;
}

/* ======================================================================== */
/* shaders                                                                   */
/* ======================================================================== */

static GLuint compile(GLenum type, const char *src, const char *tag)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        static int told;
        char log[4096];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        if (told++ < 6)
            fprintf(stderr, "[GLES] %s %s shader failed:\n%s\n--- source ---\n%s\n", tag,
                    type == GL_VERTEX_SHADER ? "vertex" : "fragment", log, src);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint gles_compile_program(const char *vs, const char *fs, const char *tag)
{
    GLuint v = compile(GL_VERTEX_SHADER, vs, tag), f, p;
    GLint ok = 0;
    if (!v) return 0;
    f = compile(GL_FRAGMENT_SHADER, fs, tag);
    if (!f) { glDeleteShader(v); return 0; }
    p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "[GLES] %s program link failed:\n%s\n", tag, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* Bind the samplers named tex0..tex3 to units 0..3 and the uniform block
 * `block` (if any) to binding 0. */
static void program_bind_units(GLuint p, const char *block)
{
    char name[8];
    int i;
    glUseProgram(p);
    for (i = 0; i < 4; i++) {
        GLint loc;
        snprintf(name, sizeof name, "tex%d", i);
        loc = glGetUniformLocation(p, name);
        if (loc >= 0) glUniform1i(loc, i);
    }
    if (block) {
        GLuint bi = glGetUniformBlockIndex(p, block);
        if (bi != GL_INVALID_INDEX) glUniformBlockBinding(p, bi, 0);
    }
}

/* ---- NV2A pass-through vertex shader (d3d8_nv2a.c's g_vs_src) ------------ */

static const char s_nv_vs[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "layout(location = 0) in vec4 a_pos;\n"
    "layout(location = 1) in vec4 a_d0;\n"
    "layout(location = 2) in vec4 a_d1;\n"
    "layout(location = 3) in float a_fog;\n"
    "layout(location = 4) in vec4 a_t0;\n"
    "layout(location = 5) in vec4 a_t1;\n"
    "layout(location = 6) in vec4 a_t2;\n"
    "layout(location = 7) in vec4 a_t3;\n"
    "uniform vec4 u_screen;\n"      /* xy: title size; z: 1 = no depth clip, emulated */
    "out vec4 v_d0; out vec4 v_d1; out float v_fog;\n"
    "out vec4 v_t0; out vec4 v_t1; out vec4 v_t2; out vec4 v_t3;\n"
    "void main() {\n"
    /* Undo the divide by w so the rasteriser interpolates perspective-correctly;
     * rhw keeps w's sign, so a vertex behind the eye is clipped at the near plane. */
    "    float w = 1.0 / a_pos.w;\n"
    "    vec4 p = vec4((a_pos.x / u_screen.x * 2.0 - 1.0) * w,\n"
    "                  (1.0 - a_pos.y / u_screen.y * 2.0) * w, a_pos.z * w, w);\n"
    "    if (u_screen.z != 0.0 && p.w > 0.0) p.z = clamp(p.z, 0.0, p.w);\n"
    "    gl_Position = vec4(p.x, -p.y, 2.0 * p.z - p.w, p.w);\n"
    "    gl_PointSize = 1.0;\n"
    "    v_d0 = a_d0; v_d1 = a_d1; v_fog = a_fog;\n"
    "    v_t0 = a_t0; v_t1 = a_t1; v_t2 = a_t2; v_t3 = a_t3;\n"
    "}\n";

#define NV_PS_CACHE 4096
static struct { uint64_t key; GLuint prog; GLint u_screen; } g_ps[NV_PS_CACHE];
static int g_nps;
volatile long g_nv_compiles = 0;
volatile double g_nv_compile_ms = 0;

static int ps_find(uint64_t key)
{
    /* Open addressing on the key; g_ps doubles as the table. */
    unsigned h = (unsigned)(key ^ (key >> 29) ^ (key >> 47)) & (NV_PS_CACHE - 1);
    unsigned n;
    for (n = 0; n < NV_PS_CACHE; n++, h = (h + 1) & (NV_PS_CACHE - 1)) {
        if (!g_ps[h].key && !g_ps[h].prog) return -1 - (int)h;      /* free slot */
        if (g_ps[h].key == key) return (int)h;
    }
    return -1 - NV_PS_CACHE;
}

int d3d8_nv2a_has_ps(unsigned long long key)
{
    int i = ps_find(key);
    return i >= 0 && g_ps[i].prog;
}

static double now_ms(void)
{
    LARGE_INTEGER f, q;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    return (double)q.QuadPart * 1000.0 / (double)f.QuadPart;
}

int d3d8_nv2a_add_ps(unsigned long long key, const char *glsl, int len)
{
    int i = ps_find(key);
    GLuint p;
    double t0;
    (void)len;
    if (i >= 0) return g_ps[i].prog != 0;
    if (i == -1 - NV_PS_CACHE || g_nps >= NV_PS_CACHE * 3 / 4) return 0;
    i = -1 - i;
    gles_check_thread("add_ps");
    t0 = now_ms();
    p = gles_compile_program(s_nv_vs, glsl, "nv2a");
    g_nv_compiles++;
    g_nv_compile_ms += now_ms() - t0;
    g_ps[i].key = key;
    g_ps[i].prog = p;
    g_nps++;
    if (!p) return 0;
    program_bind_units(p, "PshConsts");
    g_ps[i].u_screen = glGetUniformLocation(p, "u_screen");
    gles_invalidate_state();
    return 1;
}

/* ---- fixed-function shader (d3d8_shaders.c, without lighting) ------------- */

static const char s_ffp_vs[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "layout(location = 0) in vec4 a_pos;\n"
    "layout(location = 1) in vec4 a_diffuse;\n"     /* D3DCOLOR bytes: b g r a */
    "layout(location = 2) in vec4 a_specular;\n"
    "layout(location = 4) in vec2 a_t0;\n"
    "layout(location = 5) in vec2 a_t1;\n"
    "layout(location = 6) in vec2 a_t2;\n"
    "layout(location = 7) in vec2 a_t3;\n"
    "uniform mat4 u_wvp;\n"
    "uniform vec4 u_screen;\n"                      /* xy screen, z flags */
    "out vec4 v_diffuse; out vec4 v_specular; out float v_fog;\n"
    "out vec2 v_t0; out vec2 v_t1; out vec2 v_t2; out vec2 v_t3;\n"
    "void main() {\n"
    "    int fl = int(u_screen.z);\n"                /* 1 pretransformed, 2 diffuse, 4 specular */
    "    vec4 p;\n"
    "    if ((fl & 1) != 0) p = vec4(a_pos.x / u_screen.x * 2.0 - 1.0, 1.0 - a_pos.y / u_screen.y * 2.0, a_pos.z, 1.0);\n"
    "    else p = u_wvp * vec4(a_pos.xyz, 1.0);\n"
    "    gl_Position = vec4(p.x, -p.y, 2.0 * p.z - p.w, p.w);\n"
    "    gl_PointSize = 1.0;\n"
    "    v_diffuse = (fl & 2) != 0 ? a_diffuse.bgra : vec4(1.0);\n"
    "    v_specular = (fl & 4) != 0 ? a_specular.bgra : vec4(0.0);\n"
    "    v_fog = (fl & 5) == 5 ? a_specular.a : 1.0;\n"
    "    v_t0 = a_t0; v_t1 = a_t1; v_t2 = a_t2; v_t3 = a_t3;\n"
    "}\n";

static const char s_ffp_fs[] =
    "#version 300 es\n"
    "precision highp float; precision highp int;\n"
    "uniform sampler2D tex0; uniform sampler2D tex1; uniform sampler2D tex2; uniform sampler2D tex3;\n"
    "uniform vec4 u_tfactor;\n"
    "uniform vec4 u_fog_color;\n"
    "uniform vec4 u_alpha;\n"                       /* x ref (0..1), y func, z flags: 1 test 2 fog 4 spec */
    "uniform ivec4 u_stage_color[4];\n"             /* colorop, arg1, arg2, alphaop */
    "uniform ivec4 u_stage_alpha[4];\n"             /* alphaarg1, alphaarg2, bound */
    "in vec4 v_diffuse; in vec4 v_specular; in float v_fog;\n"
    "in vec2 v_t0; in vec2 v_t1; in vec2 v_t2; in vec2 v_t3;\n"
    "out vec4 o_color;\n"
    "vec4 arg(int a, vec4 cur, vec4 texel) {\n"
    "    int b = a & 15; vec4 v;\n"
    "    if (b == 0) v = v_diffuse; else if (b == 1) v = cur; else if (b == 2) v = texel;\n"
    "    else if (b == 3) v = u_tfactor; else if (b == 4) v = v_specular; else v = cur;\n"
    "    if ((a & 16) != 0) v = 1.0 - v;\n"
    "    if ((a & 32) != 0) v = v.aaaa;\n"
    "    return v;\n"
    "}\n"
    "vec4 op(int o, vec4 a1, vec4 a2, vec4 texel, vec4 cur) {\n"
    "    if (o <= 2) return a1;\n"
    "    if (o == 3) return a2;\n"
    "    if (o == 4) return a1 * a2;\n"
    "    if (o == 5) return clamp(a1 * a2 * 2.0, 0.0, 1.0);\n"
    "    if (o == 6) return clamp(a1 * a2 * 4.0, 0.0, 1.0);\n"
    "    if (o == 7) return clamp(a1 + a2, 0.0, 1.0);\n"
    "    if (o == 8) return clamp(a1 + a2 - 0.5, 0.0, 1.0);\n"
    "    if (o == 9) return clamp((a1 + a2 - 0.5) * 2.0, 0.0, 1.0);\n"
    "    if (o == 10) return clamp(a1 - a2, 0.0, 1.0);\n"
    "    if (o == 11) return clamp(a1 + a2 - a1 * a2, 0.0, 1.0);\n"
    "    if (o == 12) return mix(a2, a1, v_diffuse.a);\n"
    "    if (o == 13) return mix(a2, a1, texel.a);\n"
    "    if (o == 14) return mix(a2, a1, u_tfactor.a);\n"
    "    if (o == 15) return mix(a2, a1, cur.a);\n"
    "    if (o == 24) return vec4(clamp(4.0 * dot(a1.rgb - 0.5, a2.rgb - 0.5), 0.0, 1.0));\n"
    "    return a1 * a2;\n"
    "}\n"
    "void main() {\n"
    "    vec4 cur = v_diffuse;\n"
    "    vec4 tx[4];\n"
    "    tx[0] = texture(tex0, v_t0); tx[1] = texture(tex1, v_t1);\n"
    "    tx[2] = texture(tex2, v_t2); tx[3] = texture(tex3, v_t3);\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "        int cop = u_stage_color[i].x;\n"
    "        if (cop <= 1) break;\n"
    "        vec4 texel = u_stage_alpha[i].z != 0 ? tx[i] : vec4(1.0);\n"
    "        vec4 c = op(cop, arg(u_stage_color[i].y, cur, texel), arg(u_stage_color[i].z, cur, texel), texel, cur);\n"
    "        float a = cur.a;\n"
    "        if (u_stage_color[i].w > 1)\n"
    "            a = op(u_stage_color[i].w, arg(u_stage_alpha[i].x, cur, texel), arg(u_stage_alpha[i].y, cur, texel), texel, cur).a;\n"
    "        cur = vec4(c.rgb, a);\n"
    "    }\n"
    "    int fl = int(u_alpha.z);\n"
    "    if ((fl & 4) != 0) cur.rgb = clamp(cur.rgb + v_specular.rgb, 0.0, 1.0);\n"
    "    if ((fl & 2) != 0) cur.rgb = mix(u_fog_color.rgb, cur.rgb, v_fog);\n"
    "    if ((fl & 1) != 0) {\n"
    "        int f = int(u_alpha.y); float r = u_alpha.x; bool ok = true;\n"
    "        if (f == 1) ok = false; else if (f == 2) ok = cur.a < r; else if (f == 3) ok = cur.a == r;\n"
    "        else if (f == 4) ok = cur.a <= r; else if (f == 5) ok = cur.a > r;\n"
    "        else if (f == 6) ok = cur.a != r; else if (f == 7) ok = cur.a >= r;\n"
    "        if (!ok) discard;\n"
    "    }\n"
    "    o_color = cur;\n"
    "}\n";

static GLuint s_ffp;
static GLint  s_ffp_u_wvp, s_ffp_u_screen, s_ffp_u_tfactor, s_ffp_u_fog, s_ffp_u_alpha,
              s_ffp_u_sc, s_ffp_u_sa;

/* ---- blit (present, video) ------------------------------------------------ */

static const char s_blit_vs[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform float u_flip;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 uv = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);\n"
    "    v_uv = vec2(uv.x, u_flip != 0.0 ? 1.0 - uv.y : uv.y);\n"
    "}\n";
static const char s_blit_fs[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D tex0;\n"
    "uniform highp sampler2D tex1;\n"
    "uniform float u_lut;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    vec3 c = texture(tex0, v_uv).rgb;\n"
    "    if (u_lut != 0.0) {\n"
    "        ivec3 k = ivec3(round(clamp(c, 0.0, 1.0) * 255.0));\n"
    "        c = vec3(texelFetch(tex1, ivec2(k.r, 0), 0).r, texelFetch(tex1, ivec2(k.g, 0), 0).g,\n"
    "                 texelFetch(tex1, ivec2(k.b, 0), 0).b);\n"
    "    }\n"
    "    o_color = vec4(c, 1.0);\n"
    "}\n";

static GLuint s_blit, s_blit_smp;
static GLint  s_blit_u_flip, s_blit_u_lut;
static GLuint s_vao;

/* ======================================================================== */
/* state                                                                     */
/* ======================================================================== */

static struct {
    int valid;
    GLuint program;
    int blend, sfac, dfac, bop;
    int cmask;
    int depth_test, depth_write, depth_func;
    int stencil, sfunc, sref, smask, swmask, sfail, szfail, spass;
    int cull, front, depth_clamp;
    int scissor, sx, sy, sw, sh;
    int vx, vy, vw, vh;
    float zmin, zmax;
    GLuint tex[4], smp[4];
} S;

void gles_invalidate_state(void)
{
    memset(&S, 0, sizeof S);
}

static void use_program(GLuint p)
{
    if (!S.valid || S.program != p) { glUseProgram(p); S.program = p; }
}

static GLenum gl_blend(DWORD b)
{
    switch (b) {
    case D3DBLEND_ZERO:         return GL_ZERO;
    case D3DBLEND_ONE:          return GL_ONE;
    case D3DBLEND_SRCCOLOR:     return GL_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR:  return GL_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA:     return GL_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA:  return GL_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA:    return GL_DST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case D3DBLEND_DESTCOLOR:    return GL_DST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
    case D3DBLEND_SRCALPHASAT:  return GL_SRC_ALPHA_SATURATE;
    default:                    return GL_ONE;
    }
}

static GLenum gl_blendop(DWORD op)
{
    switch (op) {
    case 2: return GL_FUNC_SUBTRACT;
    case 3: return GL_FUNC_REVERSE_SUBTRACT;
    case 4: return GL_MIN;
    case 5: return GL_MAX;
    default: return GL_FUNC_ADD;
    }
}

static GLenum gl_cmp(DWORD c)
{
    switch (c) {
    case D3DCMP_NEVER:        return GL_NEVER;
    case D3DCMP_LESS:         return GL_LESS;
    case D3DCMP_EQUAL:        return GL_EQUAL;
    case D3DCMP_LESSEQUAL:    return GL_LEQUAL;
    case D3DCMP_GREATER:      return GL_GREATER;
    case D3DCMP_NOTEQUAL:     return GL_NOTEQUAL;
    case D3DCMP_GREATEREQUAL: return GL_GEQUAL;
    case D3DCMP_ALWAYS:       return GL_ALWAYS;
    default:                  return GL_LEQUAL;
    }
}

static GLenum gl_stencilop(DWORD op)
{
    switch (op) {
    case 2: return GL_ZERO;
    case 3: return GL_REPLACE;
    case 4: return GL_INCR;
    case 5: return GL_DECR;
    case 6: return GL_INVERT;
    case 7: return GL_INCR_WRAP;
    case 8: return GL_DECR_WRAP;
    default: return GL_KEEP;
    }
}

#define SET(field, val, call) do { int v_ = (int)(val); \
    if (!S.valid || S.field != v_) { S.field = v_; call; } } while (0)

static GLuint sampler_for(DWORD stage, int levels)
{
    #define SMP_CACHE 256
    static struct { uint32_t key; GLuint s; } cache[SMP_CACHE];
    static int n;
    const DWORD *t = g_gl.tss[stage];
    DWORD mag = t[D3DTSS_MAGFILTER], min = t[D3DTSS_MINFILTER], mip = t[D3DTSS_MIPFILTER];
    DWORD au = t[D3DTSS_ADDRESSU] ? t[D3DTSS_ADDRESSU] : 1, av = t[D3DTSS_ADDRESSV] ? t[D3DTSS_ADDRESSV] : 1;
    DWORD aniso = t[D3DTSS_MAXANISOTROPY] ? t[D3DTSS_MAXANISOTROPY] : 1;
    DWORD maxmip = t[D3DTSS_MAXMIPLEVEL];
    uint32_t key;
    int i;
    GLuint s;
    if (levels <= 1) { mip = D3DTEXF_NONE; maxmip = 0; }
    if (aniso > 16) aniso = 16;
    if ((s_host_aniso > 1 || aniso > 1) && min != D3DTEXF_POINT && min != D3DTEXF_NONE &&
        (int)aniso < s_host_aniso)
        aniso = (DWORD)s_host_aniso;
    if (min == D3DTEXF_POINT || min == D3DTEXF_NONE) aniso = 1;
    key = (mag & 7) | ((min & 7) << 3) | ((mip & 3) << 6) | ((au & 7) << 8) | ((av & 7) << 11) |
          ((aniso & 31) << 14) | ((maxmip & 15) << 19);
    for (i = 0; i < n; i++) if (cache[i].key == key) return cache[i].s;
    glGenSamplers(1, &s);
    {
        int ml = (min == D3DTEXF_LINEAR || min == D3DTEXF_ANISOTROPIC);
        int gl_mag = (mag == D3DTEXF_LINEAR || mag == D3DTEXF_ANISOTROPIC) ? GL_LINEAR : GL_NEAREST;
        int gl_min;
        if (mip == D3DTEXF_NONE) gl_min = ml ? GL_LINEAR : GL_NEAREST;
        else if (mip == D3DTEXF_LINEAR) gl_min = ml ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
        else gl_min = ml ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
        glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, gl_mag);
        glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, gl_min);
    }
    {
        DWORD a[2] = { au, av };
        GLenum pn[2] = { GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T };
        for (i = 0; i < 2; i++) {
            GLint m;
            switch (a[i]) {
            case D3DTADDRESS_MIRROR:     m = GL_MIRRORED_REPEAT; break;
            case D3DTADDRESS_CLAMP:      m = GL_CLAMP_TO_EDGE; break;
            case D3DTADDRESS_BORDER:     m = s_has_border ? GL_CLAMP_TO_BORDER_EXT : GL_CLAMP_TO_EDGE; break;
            case D3DTADDRESS_MIRRORONCE: m = s_has_mirror_once ? GL_MIRROR_CLAMP_TO_EDGE_EXT : GL_MIRRORED_REPEAT; break;
            default:                     m = GL_REPEAT; break;
            }
            glSamplerParameteri(s, pn[i], m);
        }
    }
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, (float)maxmip);
    if (mip == D3DTEXF_NONE) glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, (float)maxmip);
    if (s_has_aniso && aniso > 1)
        glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_EXT, (float)aniso < s_max_aniso ? (float)aniso : s_max_aniso);
    if (n < SMP_CACHE) { cache[n].key = key; cache[n].s = s; n++; }
    return s;
}

/* Bind the device's textures and their samplers to units 0..3. */
static void bind_textures(void)
{
    int st;
    for (st = 0; st < 4; st++) {
        GlTexture *t = (GlTexture *)g_gl.textures[st];
        GLuint tex = t ? t->tex : 0;
        GLuint smp = t ? sampler_for((DWORD)st, (int)t->levels) : 0;
        if (!S.valid || S.tex[st] != tex) {
            glActiveTexture(GL_TEXTURE0 + st);
            glBindTexture(GL_TEXTURE_2D, tex);
            S.tex[st] = tex;
        }
        if (!S.valid || S.smp[st] != smp) { glBindSampler((GLuint)st, smp); S.smp[st] = smp; }
    }
}

void gles_set_viewport_from_device(void)
{
    float sx, sy;
    const D3DVIEWPORT8 *v = &g_gl.viewport;
    int x, y, w, h;
    gles_GetGuestScale(&sx, &sy);
    x = (int)(v->X * sx + 0.5f); y = (int)(v->Y * sy + 0.5f);
    w = (int)(v->Width * sx + 0.5f); h = (int)(v->Height * sy + 0.5f);
    if (!S.valid || S.vx != x || S.vy != y || S.vw != w || S.vh != h) {
        glViewport(x, y, w, h);
        S.vx = x; S.vy = y; S.vw = w; S.vh = h;
    }
    if (!S.valid || S.zmin != v->MinZ || S.zmax != v->MaxZ) {
        glDepthRangef(v->MinZ, v->MaxZ);
        S.zmin = v->MinZ; S.zmax = v->MaxZ;
    }
}

/* The D3D8 render states as GL state. raster < 0: cull from D3DRS_CULLMODE
 * and depth clipping on (the D3D8 draws); otherwise d3d8_nv2a_draw's bits:
 * 0 depth clip, 2:1 cull (0 none, 1 front, 2 back), 3 front face CCW (D3D). */
void gles_apply_states(int raster)
{
    const DWORD *rs = g_gl.rs;
    int cull, front_ccw, clip;
    glBindFramebuffer(GL_FRAMEBUFFER, gles_draw_target());

    SET(blend, rs[D3DRS_ALPHABLENDENABLE] != 0,
        (S.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND)));
    if (S.blend) {
        int sf = (int)gl_blend(rs[D3DRS_SRCBLEND]), df = (int)gl_blend(rs[D3DRS_DESTBLEND]);
        int bo = (int)gl_blendop(rs[D3DRS_BLENDOP] ? rs[D3DRS_BLENDOP] : 1);
        if (!S.valid || S.sfac != sf || S.dfac != df) { glBlendFunc((GLenum)sf, (GLenum)df); S.sfac = sf; S.dfac = df; }
        SET(bop, bo, glBlendEquation((GLenum)bo));
    }
    SET(cmask, rs[D3DRS_COLORWRITEENABLE] & 0xF,
        glColorMask((S.cmask & 1) != 0, (S.cmask & 2) != 0, (S.cmask & 4) != 0, (S.cmask & 8) != 0));

    SET(depth_test, rs[D3DRS_ZENABLE] != 0, (S.depth_test ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST)));
    SET(depth_write, rs[D3DRS_ZWRITEENABLE] != 0, glDepthMask(S.depth_write ? GL_TRUE : GL_FALSE));
    SET(depth_func, gl_cmp(rs[D3DRS_ZFUNC]), glDepthFunc((GLenum)S.depth_func));

    SET(stencil, rs[D3DRS_STENCILENABLE] != 0, (S.stencil ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST)));
    if (S.stencil) {
        int f = (int)gl_cmp(rs[D3DRS_STENCILFUNC]), r = (int)(rs[D3DRS_STENCILREF] & 0xFF),
            m = (int)(rs[D3DRS_STENCILMASK] & 0xFF), wm = (int)(rs[D3DRS_STENCILWRITEMASK] & 0xFF);
        int fo = (int)gl_stencilop(rs[D3DRS_STENCILFAIL]), zo = (int)gl_stencilop(rs[D3DRS_STENCILZFAIL]),
            po = (int)gl_stencilop(rs[D3DRS_STENCILPASS]);
        if (!S.valid || S.sfunc != f || S.sref != r || S.smask != m) {
            glStencilFunc((GLenum)f, r, (GLuint)m); S.sfunc = f; S.sref = r; S.smask = m;
        }
        SET(swmask, wm, glStencilMask((GLuint)S.swmask));
        if (!S.valid || S.sfail != fo || S.szfail != zo || S.spass != po) {
            glStencilOp((GLenum)fo, (GLenum)zo, (GLenum)po); S.sfail = fo; S.szfail = zo; S.spass = po;
        }
    } else {
        SET(swmask, 0xFF, glStencilMask(0xFF));
    }

    if (raster < 0) {
        DWORD cm = rs[D3DRS_CULLMODE];
        cull = cm == D3DCULL_NONE ? 0 : cm == D3DCULL_CW ? 1 : 2;   /* as d3d8_states.c */
        front_ccw = 0;
        clip = 1;
    } else {
        cull = (raster >> 1) & 3;
        front_ccw = (raster >> 3) & 1;
        clip = raster & 1;
    }
    SET(cull, cull, (S.cull ? (glEnable(GL_CULL_FACE), glCullFace(S.cull == 1 ? GL_FRONT : GL_BACK))
                            : glDisable(GL_CULL_FACE)));
    /* D3D's clockwise is GL's counter-clockwise here (see gles_internal.h). */
    SET(front, front_ccw, glFrontFace(S.front ? GL_CW : GL_CCW));
    if (s_has_depth_clamp)
        SET(depth_clamp, !clip, (S.depth_clamp ? glEnable(GL_DEPTH_CLAMP_EXT) : glDisable(GL_DEPTH_CLAMP_EXT)));

    if (g_gles_wclip_on) {
        float sx, sy;
        int x, y, w, h;
        gles_GetGuestScale(&sx, &sy);
        x = (int)(g_gles_wclip[0] * sx + 0.5f);
        y = (int)(g_gles_wclip[1] * sy + 0.5f);
        w = (int)(g_gles_wclip[2] * sx + 0.5f) - x;
        h = (int)(g_gles_wclip[3] * sy + 0.5f) - y;
        SET(scissor, 1, glEnable(GL_SCISSOR_TEST));
        if (!S.valid || S.sx != x || S.sy != y || S.sw != w || S.sh != h) {
            glScissor(x, y, w < 0 ? 0 : w, h < 0 ? 0 : h); S.sx = x; S.sy = y; S.sw = w; S.sh = h;
        }
    } else {
        SET(scissor, 0, glDisable(GL_SCISSOR_TEST));
    }
    gles_set_viewport_from_device();
    bind_textures();
    S.valid = 1;
}

/* ======================================================================== */
/* init                                                                      */
/* ======================================================================== */

void d3d8_SetAnisotropy(int n) { s_host_aniso = (n == 2 || n == 4 || n == 8 || n == 16) ? n : 0; }
int  d3d8_GetAnisotropy(void)  { return s_host_aniso; }

int gles_draw_init(void)
{
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    if (!ext) ext = "";
    s_has_depth_clamp = strstr(ext, "GL_EXT_depth_clamp") != NULL;
    s_has_aniso = strstr(ext, "GL_EXT_texture_filter_anisotropic") != NULL;
    s_has_border = strstr(ext, "GL_EXT_texture_border_clamp") != NULL ||
                   strstr(ext, "GL_OES_texture_border_clamp") != NULL;
    s_has_mirror_once = strstr(ext, "GL_EXT_texture_mirror_clamp_to_edge") != NULL;
    if (s_has_aniso) glGetFloatv(0x84FF /* MAX_TEXTURE_MAX_ANISOTROPY */, &s_max_aniso);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &s_ubo_align);
    if (s_ubo_align < 16) s_ubo_align = 16;
    fprintf(stderr, "[GLES] %s / %s / %s\n", (const char *)glGetString(GL_VENDOR),
            (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    fprintf(stderr, "[GLES] depth clamp %s, anisotropy %s (%.0fx), border clamp %s\n",
            s_has_depth_clamp ? "yes" : "emulated", s_has_aniso ? "yes" : "no", s_max_aniso,
            s_has_border ? "yes" : "no");

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);

    s_ffp = gles_compile_program(s_ffp_vs, s_ffp_fs, "fixed-function");
    if (!s_ffp) return 0;
    program_bind_units(s_ffp, NULL);
    s_ffp_u_wvp = glGetUniformLocation(s_ffp, "u_wvp");
    s_ffp_u_screen = glGetUniformLocation(s_ffp, "u_screen");
    s_ffp_u_tfactor = glGetUniformLocation(s_ffp, "u_tfactor");
    s_ffp_u_fog = glGetUniformLocation(s_ffp, "u_fog_color");
    s_ffp_u_alpha = glGetUniformLocation(s_ffp, "u_alpha");
    s_ffp_u_sc = glGetUniformLocation(s_ffp, "u_stage_color");
    s_ffp_u_sa = glGetUniformLocation(s_ffp, "u_stage_alpha");

    s_blit = gles_compile_program(s_blit_vs, s_blit_fs, "blit");
    if (!s_blit) return 0;
    program_bind_units(s_blit, NULL);
    s_blit_u_flip = glGetUniformLocation(s_blit, "u_flip");
    s_blit_u_lut = glGetUniformLocation(s_blit, "u_lut");
    glGenSamplers(1, &s_blit_smp);
    glSamplerParameteri(s_blit_smp, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(s_blit_smp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(s_blit_smp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(s_blit_smp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gles_invalidate_state();
    return 1;
}

void gles_draw_shutdown(void) {}

/* ======================================================================== */
/* draws                                                                     */
/* ======================================================================== */

static GLenum gl_prim(D3DPRIMITIVETYPE p)
{
    switch (p) {
    case D3DPT_POINTLIST:     return GL_POINTS;
    case D3DPT_LINELIST:      return GL_LINES;
    case D3DPT_LINESTRIP:     return GL_LINE_STRIP;
    case D3DPT_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
    case D3DPT_TRIANGLEFAN:   return GL_TRIANGLE_FAN;
    default:                  return GL_TRIANGLES;
    }
}

static UINT prim_verts(D3DPRIMITIVETYPE p, UINT n)
{
    switch (p) {
    case D3DPT_POINTLIST:     return n;
    case D3DPT_LINELIST:      return n * 2;
    case D3DPT_LINESTRIP:     return n + 1;
    case D3DPT_TRIANGLELIST:  return n * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:   return n + 2;
    case D3DPT_QUADLIST:      return n * 4;
    default:                  return 0;
    }
}

static void disable_attribs(int from)
{
    int i;
    for (i = from; i < 8; i++) glDisableVertexAttribArray((GLuint)i);
}

HRESULT d3d8_nv2a_draw(D3DPRIMITIVETYPE prim, UINT prim_count, const void *verts, UINT stride,
                       unsigned long long ps_key, const void *ps_consts, UINT ps_consts_size,
                       int raster)
{
    int i = ps_find(ps_key);
    UINT nv = prim_verts(prim, prim_count);
    GLintptr voff, uoff;
    GLuint prog;
    float screen[4];
    if (i < 0 || !(prog = g_ps[i].prog) || !nv) return E_FAIL;
    if (prim != D3DPT_TRIANGLELIST && prim != D3DPT_LINELIST && prim != D3DPT_LINESTRIP &&
        prim != D3DPT_POINTLIST)
        return E_INVALIDARG;
    gles_check_thread("nv2a_draw");

    gles_apply_states(raster);
    use_program(prog);
    screen[0] = (float)d3d8_GetBackbufferWidth();
    screen[1] = (float)d3d8_GetBackbufferHeight();
    if (screen[0] <= 0.0f) screen[0] = 640.0f;
    if (screen[1] <= 0.0f) screen[1] = 480.0f;
    screen[2] = (!(raster & 1) && !s_has_depth_clamp) ? 1.0f : 0.0f;
    screen[3] = 0.0f;
    glUniform4fv(g_ps[i].u_screen, 1, screen);

    uoff = stream_put(&s_ubo, ps_consts, (GLsizeiptr)ps_consts_size, s_ubo_align);
    if (uoff < 0) return E_OUTOFMEMORY;
    glBindBufferRange(GL_UNIFORM_BUFFER, 0, s_ubo.buf, uoff, (GLsizeiptr)ps_consts_size);

    voff = stream_put(&s_vtx, verts, (GLsizeiptr)nv * stride, 16);
    if (voff < 0) return E_OUTOFMEMORY;
    glBindVertexArray(s_vao);
    {
        /* ProgVertex: pos 0, d0 16, d1 32, fog 48, t0..t3 52.. */
        static const int off[8] = { 0, 16, 32, 48, 52, 68, 84, 100 };
        static const int cnt[8] = { 4, 4, 4, 1, 4, 4, 4, 4 };
        int a;
        for (a = 0; a < 8; a++) {
            glEnableVertexAttribArray((GLuint)a);
            glVertexAttribPointer((GLuint)a, cnt[a], GL_FLOAT, GL_FALSE, (GLsizei)stride,
                                  (const void *)(voff + off[a]));
        }
    }
    glDrawArrays(gl_prim(prim), 0, (GLsizei)nv);
    return S_OK;
}

/* Vertex programs on the GPU need a GLSL vertex-program generator and, for
 * points, geometry the ES 3.0 pipeline cannot make: not in this build yet --
 * the translator then runs the programs on the CPU and draws through
 * d3d8_nv2a_draw, as XBOX_VSH_GPU=0 does on Windows. */
#include "../d3d8_nv2a_vsh.h"
int d3d8_nv2a_vsh_ready(const Nv2aVshDraw *d) { (void)d; return 0; }
int d3d8_nv2a_draw_program_gpu(const Nv2aVshDraw *d) { (void)d; return 0; }
int d3d8_points_gpu_on(void) { return 0; }
int d3d8_points_check_on(void) { return 0; }
void d3d8_nv2a_points_expect(const void *v, unsigned n, unsigned s) { (void)v; (void)n; (void)s; }

/* ---- fixed function ------------------------------------------------------- */

static void ffp_uniforms(DWORD fvf)
{
    const DWORD *rs = g_gl.rs;
    float screen[4], tf[4], fog[4] = { 0, 0, 0, 1 }, alpha[4] = { 0, 0, 0, 0 };
    GLint sc[16], sa[16];
    DWORD c = rs[D3DRS_TEXTUREFACTOR];
    int st, flags = 0;
    screen[0] = (float)d3d8_GetBackbufferWidth();
    screen[1] = (float)d3d8_GetBackbufferHeight();
    if (fvf & D3DFVF_XYZRHW) flags |= 1;
    if (fvf & D3DFVF_DIFFUSE) flags |= 2;
    if (fvf & D3DFVF_SPECULAR) flags |= 4;
    screen[2] = (float)flags;
    screen[3] = 0.0f;
    glUniform4fv(s_ffp_u_screen, 1, screen);
    if (!(fvf & D3DFVF_XYZRHW)) {
        /* World * View * Projection, row vectors (D3D) = column-major GL as is. */
        const D3DMATRIX *w = &g_gl.transforms[256], *v = &g_gl.transforms[2], *p = &g_gl.transforms[3];
        float wv[16], wvp[16];
        int r, k, j;
        for (r = 0; r < 4; r++) for (k = 0; k < 4; k++) {
            float s = 0; for (j = 0; j < 4; j++) s += w->m[r][j] * v->m[j][k]; wv[r * 4 + k] = s; }
        for (r = 0; r < 4; r++) for (k = 0; k < 4; k++) {
            float s = 0; for (j = 0; j < 4; j++) s += wv[r * 4 + j] * p->m[j][k]; wvp[r * 4 + k] = s; }
        glUniformMatrix4fv(s_ffp_u_wvp, 1, GL_FALSE, wvp);
    }
    tf[0] = ((c >> 16) & 0xFF) / 255.0f; tf[1] = ((c >> 8) & 0xFF) / 255.0f;
    tf[2] = (c & 0xFF) / 255.0f; tf[3] = ((c >> 24) & 0xFF) / 255.0f;
    glUniform4fv(s_ffp_u_tfactor, 1, tf);
    if (rs[D3DRS_FOGENABLE]) {
        DWORD fc = rs[D3DRS_FOGCOLOR];
        fog[0] = ((fc >> 16) & 0xFF) / 255.0f; fog[1] = ((fc >> 8) & 0xFF) / 255.0f; fog[2] = (fc & 0xFF) / 255.0f;
        alpha[2] += 2.0f;
    }
    glUniform4fv(s_ffp_u_fog, 1, fog);
    if (rs[D3DRS_ALPHATESTENABLE]) {
        alpha[0] = (float)(rs[D3DRS_ALPHAREF] & 0xFF) / 255.0f;
        alpha[1] = (float)rs[D3DRS_ALPHAFUNC];
        alpha[2] += 1.0f;
    }
    if (rs[D3DRS_SPECULARENABLE]) alpha[2] += 4.0f;
    glUniform4fv(s_ffp_u_alpha, 1, alpha);
    for (st = 0; st < 4; st++) {
        const DWORD *t = g_gl.tss[st];
        sc[st * 4 + 0] = (GLint)t[D3DTSS_COLOROP];
        sc[st * 4 + 1] = (GLint)t[D3DTSS_COLORARG1];
        sc[st * 4 + 2] = (GLint)t[D3DTSS_COLORARG2];
        sc[st * 4 + 3] = (GLint)t[D3DTSS_ALPHAOP];
        sa[st * 4 + 0] = (GLint)t[D3DTSS_ALPHAARG1];
        sa[st * 4 + 1] = (GLint)t[D3DTSS_ALPHAARG2];
        sa[st * 4 + 2] = g_gl.textures[st] != NULL;
        sa[st * 4 + 3] = 0;
    }
    glUniform4iv(s_ffp_u_sc, 4, sc);
    glUniform4iv(s_ffp_u_sa, 4, sa);
}

/* Attribute pointers for an FVF at `base` in the vertex stream. */
static void ffp_attribs(DWORD fvf, UINT stride, GLintptr base)
{
    GLintptr off = base;
    int ntex = (int)((fvf >> 8) & 0xF), t;
    disable_attribs(0);
    if (fvf & D3DFVF_XYZRHW) {
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, (GLsizei)stride, (const void *)off);
        off += 16;
    } else {
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, (GLsizei)stride, (const void *)off);
        off += 12;
    }
    if (fvf & D3DFVF_NORMAL) off += 12;
    if (fvf & D3DFVF_DIFFUSE) {
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, (const void *)off);
        off += 4;
    }
    if (fvf & D3DFVF_SPECULAR) {
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, (const void *)off);
        off += 4;
    }
    for (t = 0; t < ntex && t < 4; t++) {
        glEnableVertexAttribArray((GLuint)(4 + t));
        glVertexAttribPointer((GLuint)(4 + t), 2, GL_FLOAT, GL_FALSE, (GLsizei)stride, (const void *)off);
        off += 8;
    }
}

HRESULT gles_draw_ffp(D3DPRIMITIVETYPE prim, UINT prim_count, const void *verts, UINT stride,
                      const void *indices, int index_bytes, UINT nverts)
{
    DWORD fvf = g_gl.vertex_shader;
    UINT n = prim_verts(prim, prim_count);
    GLintptr voff;
    void *quad = NULL;
    if (!n || !verts || !stride) return E_INVALIDARG;
    if (!s_ffp) return E_FAIL;
    gles_check_thread("DrawPrimitiveUP");

    if (prim == D3DPT_QUADLIST && !indices) {
        /* No quads in GL: two triangles each. */
        UINT q, k;
        static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
        quad = malloc((size_t)prim_count * 6 * stride);
        if (!quad) return E_OUTOFMEMORY;
        for (q = 0; q < prim_count; q++)
            for (k = 0; k < 6; k++)
                memcpy((uint8_t *)quad + (q * 6 + k) * stride,
                       (const uint8_t *)verts + (q * 4 + tri[k]) * stride, stride);
        verts = quad;
        n = prim_count * 6;
        prim = D3DPT_TRIANGLELIST;
    }

    gles_apply_states(-1);
    use_program(s_ffp);
    ffp_uniforms(fvf);
    glBindVertexArray(s_vao);
    {
        UINT vbytes = (indices ? nverts : n) * stride;
        voff = stream_put(&s_vtx, verts, (GLsizeiptr)vbytes, 16);
    }
    free(quad);
    if (voff < 0) return E_OUTOFMEMORY;
    ffp_attribs(fvf, stride, voff);
    if (indices) {
        GLintptr ioff = stream_put(&s_idx, indices, (GLsizeiptr)n * index_bytes, 4);
        if (ioff < 0) return E_OUTOFMEMORY;
        glDrawElements(gl_prim(prim), (GLsizei)n, index_bytes == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                       (const void *)ioff);
    } else {
        glDrawArrays(gl_prim(prim), 0, (GLsizei)n);
    }
    return S_OK;
}

/* ---- blit ----------------------------------------------------------------- */

void gles_blit(GLuint tex, int x, int y, int w, int h, int flip_v, GLuint lut)
{
    if (!s_blit) return;
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthRangef(0.0f, 1.0f);
    glViewport(x, y, w, h);
    glUseProgram(s_blit);
    glUniform1f(s_blit_u_flip, flip_v ? 1.0f : 0.0f);
    glUniform1f(s_blit_u_lut, lut ? 1.0f : 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, s_blit_smp);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, lut);
    glBindSampler(1, 0);
    glBindVertexArray(s_vao);
    disable_attribs(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    gles_invalidate_state();
}

/* Reserve stream room up front for a multi-part upload (unused for now). */
void gles_stream_reserve(GLsizeiptr bytes) { stream_reserve(&s_vtx, bytes); }
