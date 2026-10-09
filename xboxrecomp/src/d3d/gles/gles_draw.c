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
#include "../../kernel/xbox_perf.h"
#include "../../nv2a/nv2a_psh.h"
#include "gles_vsh.h"
#include "gles_progcache.h"
#include "gles_psh_uber.h"
#include <dxgiformat.h>
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
    unsigned gen;                 /* +1 at each orphaning: older offsets are gone */
    unsigned bound;               /* s_epoch when last bound to its target */
} Stream;

static unsigned s_epoch = 1;      /* +1 at each gles_invalidate_state */

static void stream_bind(Stream *s)
{
    if (s->bound != s_epoch) { glBindBuffer(s->target, s->buf); s->bound = s_epoch; }
}

/* 1: glBufferSubData; 0: unsynchronized glMapBufferRange. Mapping is the cheap
 * path on phone drivers; an emulator's GL pipe makes each map a round trip.
 * OT_GL_UPLOAD=map|sub overrides the choice of gles_draw_init. */
static int s_upload_sub;

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
        stream_bind(s);
        glBufferData(s->target, s->size, NULL, GL_STREAM_DRAW);
    } else {
        stream_bind(s);
    }
    if (n > s->size) return -1;
    off = (s->off + align - 1) / align * align;
    if (off + n > s->size) {
        glBufferData(s->target, s->size, NULL, GL_STREAM_DRAW);   /* orphan */
        off = 0;
        s->gen++;
    }
    if (s_upload_sub) {
        glBufferSubData(s->target, off, n, data);
        s->off = off + n;
        return off;
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
    if (s->buf && s->off + total > s->size) {
        stream_bind(s);
        glBufferData(s->target, s->size, NULL, GL_STREAM_DRAW);
        s->off = 0;
        s->gen++;
    }
}

/* ======================================================================== */
/* shaders                                                                   */
/* ======================================================================== */


GLuint gles_compile_program(const char *vs, const char *fs, const char *tag)
{
    return pc_link_sync(vs, fs, tag);
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
/* state: 1 building (job), 2 ready, 3 failed. Until 2, draws use the
 * ubershader with `uber` (if has_uber). */
static struct {
    uint64_t key; GLuint prog; char *src; GLint u_screen; float screen[4];
    int state, has_uber, checked; PcJob *job;
    uint32_t uber[GLES_PSH_UBER_WORDS];
} g_ps[NV_PS_CACHE];

/* The ubershader program (s_nv_vs + gles_psh_uber_fs); OT_PSH_UBER=1 draws
 * everything with it (to compare). */
static GLuint s_uber;
static GLint s_uber_u_screen;
static float s_uber_screen[4];
static int s_uber_all;
static int s_uber_check;            /* OT_PSH_UBER_CHECK=1: compare it with each real shader */

/* d3d8_nv2a_ps_state's hand-over to the next add_ps. */
static uint64_t s_pend_key;
static int s_pend_ok;
static uint32_t s_pend_uber[GLES_PSH_UBER_WORDS];

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
    return i >= 0 && (g_ps[i].state != 3 || g_ps[i].has_uber);
}

void d3d8_nv2a_ps_state(unsigned long long key, const void *state)
{
    s_pend_key = key;
    s_pend_ok = state && gles_psh_uber_pack((const Nv2aPshState *)state, s_pend_uber);
}

static void ps_ready(int i, GLuint p)
{
    g_ps[i].prog = p;
    g_ps[i].state = p ? 2 : 3;
    if (!p) return;
    program_bind_units(p, "PshConsts");
    g_ps[i].u_screen = glGetUniformLocation(p, "u_screen");
    gles_invalidate_state();
}

/* 1 when combiner shader i is ready to draw with. */
static int ps_poll(int i)
{
    GLuint p;
    if (g_ps[i].state == 1 && pc_job_poll(g_ps[i].job, &p)) {
        g_ps[i].job = NULL;
        ps_ready(i, p);
    }
    return g_ps[i].state == 2 && !(s_uber_all && g_ps[i].has_uber);
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
    if (i >= 0) return g_ps[i].state != 3 || g_ps[i].has_uber;
    if (i == -1 - NV_PS_CACHE || g_nps >= NV_PS_CACHE * 3 / 4) return 0;
    i = -1 - i;
    gles_check_thread("add_ps");
    t0 = now_ms();
    g_ps[i].key = key;
    g_ps[i].src = strdup(glsl);
    g_ps[i].has_uber = s_uber && s_pend_ok && s_pend_key == key;
    if (g_ps[i].has_uber) memcpy(g_ps[i].uber, s_pend_uber, sizeof g_ps[i].uber);
    s_pend_ok = 0;
    g_nps++;
    /* From the disk cache; else built on the worker while the ubershader
     * draws; else (no worker, or no ubershader for it) here. */
    p = g_ps[i].src ? pc_load_cached(s_nv_vs, g_ps[i].src) : 0;
    if (!p && g_ps[i].src && g_ps[i].has_uber &&
        (g_ps[i].job = pc_link_async(s_nv_vs, g_ps[i].src, "nv2a"))) {
        g_ps[i].state = 1;
    } else {
        if (!p && g_ps[i].src) {
            p = pc_link_sync(s_nv_vs, g_ps[i].src, "nv2a");
            g_nv_compiles++;
        }
        ps_ready(i, p);
    }
    g_nv_compile_ms += now_ms() - t0;
    return g_ps[i].state != 3 || g_ps[i].has_uber;
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
    /* vertex attribute arrays: the enabled set (amask_ok) and the layout the
     * pointers describe (0: unknown) */
    unsigned amask; int amask_ok;
    uint64_t layout;
} S;

/* Attributes whose current value (the one a disabled array reads) a
 * vertex-program draw set: put back to (0, 0, 0, 1) for the other shaders. */
static unsigned s_cur_dirty;

static void attribs_mask(unsigned mask)
{
    unsigned ch = S.amask_ok ? (S.amask ^ mask) : 0xFFFFu;
    int a;
    for (a = 0; ch; a++, ch >>= 1)
        if (ch & 1) {
            if (mask & (1u << a)) glEnableVertexAttribArray((GLuint)a);
            else glDisableVertexAttribArray((GLuint)a);
        }
    S.amask = mask;
    S.amask_ok = 1;
}

static void attribs_clean_current(void)
{
    int a;
    for (a = 0; s_cur_dirty; a++, s_cur_dirty >>= 1)
        if (s_cur_dirty & 1) glVertexAttrib4f((GLuint)a, 0.0f, 0.0f, 0.0f, 1.0f);
}

void gles_invalidate_state(void)
{
    memset(&S, 0, sizeof S);
    s_epoch++;
}

/* Uniform blocks: 0 combiner constants, 1 vertex-program constants, 2 their
 * parameters. Draws in a row mostly repeat them: then the range bound last
 * is kept. Callers reserve s_ubo for the whole draw first, so no upload of
 * the draw orphans the buffer an earlier binding of it points into. */
static struct { uint8_t last[VSHCPU_CONSTANTS * 16]; unsigned size, gen, epoch; } s_us[4];

static int ubo_bind(GLuint binding, const void *data, unsigned size)
{
    GLintptr off;
    static int nodedup = -1;
    if (nodedup < 0) nodedup = getenv("OT_UBO_NODEDUP") != NULL;
    if (!nodedup && s_us[binding].epoch == s_epoch && s_us[binding].gen == s_ubo.gen &&
        s_us[binding].size == size && !memcmp(s_us[binding].last, data, size))
        return 1;
    off = stream_put(&s_ubo, data, (GLsizeiptr)size, s_ubo_align);
    if (off < 0) return 0;
    glBindBufferRange(GL_UNIFORM_BUFFER, binding, s_ubo.buf, off, (GLsizeiptr)size);
    if (size <= sizeof s_us[binding].last) {
        memcpy(s_us[binding].last, data, size);
        s_us[binding].size = size;
        s_us[binding].gen = s_ubo.gen;
        s_us[binding].epoch = s_epoch;
    } else {
        s_us[binding].epoch = 0;
    }
    return 1;
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
    pc_init();
    {
        const char *e = getenv("OT_PSH_UBER");
        s_uber_all = e && e[0] == '1';
        s_uber_check = getenv("OT_PSH_UBER_CHECK") != NULL;
        if (!(e && e[0] == '0')) {
            s_uber = pc_link_sync(s_nv_vs, gles_psh_uber_fs, "combiner ubershader");
            if (s_uber) {
                GLuint bi;
                program_bind_units(s_uber, "PshConsts");
                bi = glGetUniformBlockIndex(s_uber, "PshUber");
                if (bi != GL_INVALID_INDEX) glUniformBlockBinding(s_uber, bi, 3);
                s_uber_u_screen = glGetUniformLocation(s_uber, "u_screen");
            } else
                fprintf(stderr, "[SHADERS] no combiner ubershader: new combiner shaders build on the render thread\n");
        }
    }
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    if (!ext) ext = "";
    s_has_depth_clamp = strstr(ext, "GL_EXT_depth_clamp") != NULL;
    s_has_aniso = strstr(ext, "GL_EXT_texture_filter_anisotropic") != NULL;
    s_has_border = strstr(ext, "GL_EXT_texture_border_clamp") != NULL ||
                   strstr(ext, "GL_OES_texture_border_clamp") != NULL;
    s_has_mirror_once = strstr(ext, "GL_EXT_texture_mirror_clamp_to_edge") != NULL;
    if (s_has_aniso) glGetFloatv(0x84FF /* MAX_TEXTURE_MAX_ANISOTROPY */, &s_max_aniso);
    {
        const char *r = (const char *)glGetString(GL_RENDERER), *e = getenv("OT_GL_UPLOAD");
        if (!r) r = "";
        s_upload_sub = strstr(r, "Emulator") || strstr(r, "SwiftShader") || strstr(r, "ANGLE");
        if (e && !strcmp(e, "sub")) s_upload_sub = 1;
        if (e && !strcmp(e, "map")) s_upload_sub = 0;
    }
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &s_ubo_align);
    if (s_ubo_align < 16) s_ubo_align = 16;
    fprintf(stderr, "[GLES] %s / %s / %s\n", (const char *)glGetString(GL_VENDOR),
            (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    fprintf(stderr, "[GLES] depth clamp %s, anisotropy %s (%.0fx), border clamp %s\n",
            s_has_depth_clamp ? "yes" : "emulated", s_has_aniso ? "yes" : "no", s_max_aniso,
            s_has_border ? "yes" : "no");
    fprintf(stderr, "[GLES] buffer uploads: %s\n", s_upload_sub ? "glBufferSubData" : "mapped");

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

/* OT_PSH_UBER_CHECK=1 (diagnostic): draw this draw with combiner shader i and
 * with the ubershader into a scratch target (no blending, depth or stencil;
 * same viewport and scissor), read both back and log how far they differ. */
static void uber_check(int i, GLenum mode, GLint first, GLsizei count, int raster)
{
    static GLuint fbo, tex;
    static int tw, th;
    static unsigned long long checks, bad;
    static uint8_t *pa, *pb;
    GLint vp[4];
    size_t n, k;
    int maxd = 0, differ = 0, covered = 0;
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0) return;
    if (!fbo || tw < vp[0] + vp[2] || th < vp[1] + vp[3]) {
        if (!fbo) { glGenFramebuffers(1, &fbo); glGenTextures(1, &tex); }
        else { glDeleteTextures(1, &tex); glGenTextures(1, &tex); }
        tw = vp[0] + vp[2]; th = vp[1] + vp[3];
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, tw, th);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        free(pa); free(pb);
        pa = (uint8_t *)malloc((size_t)tw * th * 4);
        pb = (uint8_t *)malloc((size_t)tw * th * 4);
        gles_invalidate_state();
        gles_apply_states(raster);
    }
    if (!pa || !pb) return;
    n = (size_t)vp[2] * vp[3] * 4;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(g_ps[i].prog);
    glDrawArrays(mode, first, count);
    glReadPixels(vp[0], vp[1], vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, pa);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(s_uber);
    if (memcmp(s_uber_screen, g_ps[i].screen, sizeof s_uber_screen)) {
        memcpy(s_uber_screen, g_ps[i].screen, sizeof s_uber_screen);
        glUniform4fv(s_uber_u_screen, 1, s_uber_screen);
    }
    ubo_bind(3, g_ps[i].uber, sizeof g_ps[i].uber);
    glDrawArrays(mode, first, count);
    glReadPixels(vp[0], vp[1], vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, pb);
    for (k = 0; k < n; k += 4) {
        int d = 0, c;
        if (pa[k + 3] || pb[k + 3] || pa[k] || pb[k]) covered++;
        for (c = 0; c < 4; c++) {
            int e = abs((int)pa[k + c] - (int)pb[k + c]);
            if (e > d) d = e;
        }
        if (d > maxd) maxd = d;
        if (d > 2) differ++;
    }
    checks++;
    if (differ) bad++;
    {
        /* OT_PSH_UBER_DUMP=dir: the first differing pairs as PPM (RGB, then alpha). */
        static int dumped;
        const char *dir = getenv("OT_PSH_UBER_DUMP");
        if (dir && differ && dumped < 6) {
            int w, which;
            for (which = 0; which < 2; which++) {
                const uint8_t *px = which ? pb : pa;
                char path[600];
                FILE *f;
                snprintf(path, sizeof path, "%s/check%d_%016llX_%s.ppm", dir, dumped,
                         (unsigned long long)g_ps[i].key, which ? "uber" : "real");
                if (!(f = fopen(path, "wb"))) continue;
                fprintf(f, "P6\n%d %d\n255\n", vp[2] * 2, vp[3]);
                for (w = vp[3] - 1; w >= 0; w--) {
                    int x;
                    for (x = 0; x < vp[2]; x++) fwrite(px + ((size_t)w * vp[2] + x) * 4, 1, 3, f);
                    for (x = 0; x < vp[2]; x++) {
                        uint8_t a3[3];
                        a3[0] = a3[1] = a3[2] = px[((size_t)w * vp[2] + x) * 4 + 3];
                        fwrite(a3, 1, 3, f);
                    }
                }
                fclose(f);
            }
            fprintf(stderr, "[UBER-CHECK] dumped pair %d\n", dumped);
            dumped++;
        }
    }
    if (differ || (checks % 10) == 1)
        fprintf(stderr, "[UBER-CHECK] shader %016llX: %d of %d covered pixels differ by more than 2/255 "
                "(max %d); %llu checked, %llu differ\n", (unsigned long long)g_ps[i].key, differ, covered,
                maxd, checks, bad);
    gles_invalidate_state();
    gles_apply_states(raster);
}

HRESULT d3d8_nv2a_draw(D3DPRIMITIVETYPE prim, UINT prim_count, const void *verts, UINT stride,
                       unsigned long long ps_key, const void *ps_consts, UINT ps_consts_size,
                       int raster)
{
    int i = ps_find(ps_key), uber;
    UINT nv = prim_verts(prim, prim_count);
    GLintptr voff;
    GLuint prog;
    GLint u_screen;
    float screen[4], *screen_set;
    if (i < 0 || !nv) return E_FAIL;
    uber = !ps_poll(i);
    if (!uber) {
        prog = g_ps[i].prog; u_screen = g_ps[i].u_screen; screen_set = g_ps[i].screen;
    } else if (g_ps[i].has_uber) {
        prog = s_uber; u_screen = s_uber_u_screen; screen_set = s_uber_screen;
        pc_note_fallback();
    } else
        return E_FAIL;
    if (prim != D3DPT_TRIANGLELIST && prim != D3DPT_LINELIST && prim != D3DPT_LINESTRIP &&
        prim != D3DPT_POINTLIST)
        return E_INVALIDARG;
    gles_check_thread("nv2a_draw");
    double pt = g_perf_on ? perf_now() : 0.0;
#define PERF_STEP(z) do { if (g_perf_on) { double t_ = perf_now(); perf_add((z), t_ - pt); pt = t_; } } while (0)

    gles_apply_states(raster);
    use_program(prog);
    screen[0] = (float)d3d8_GetBackbufferWidth();
    screen[1] = (float)d3d8_GetBackbufferHeight();
    if (screen[0] <= 0.0f) screen[0] = 640.0f;
    if (screen[1] <= 0.0f) screen[1] = 480.0f;
    screen[2] = (!(raster & 1) && !s_has_depth_clamp) ? 1.0f : 0.0f;
    screen[3] = 0.0f;
    if (memcmp(screen_set, screen, sizeof screen)) {
        glUniform4fv(u_screen, 1, screen);
        memcpy(screen_set, screen, sizeof screen);
    }
    PERF_STEP(PZ_DSTATE);

    stream_reserve(&s_ubo, (GLsizeiptr)ps_consts_size + sizeof g_ps[i].uber + 2 * s_ubo_align);
    if (!ubo_bind(0, ps_consts, ps_consts_size)) return E_OUTOFMEMORY;
    if (uber && !ubo_bind(3, g_ps[i].uber, sizeof g_ps[i].uber)) return E_OUTOFMEMORY;
    PERF_STEP(PZ_DCB);

    /* The vertices at a multiple of the stride, so the attributes point at the
     * buffer's start once and the draw's first vertex says where they are. */
    voff = stream_put(&s_vtx, verts, (GLsizeiptr)nv * stride, (GLsizeiptr)stride);
    if (voff < 0) return E_OUTOFMEMORY;
    glBindVertexArray(s_vao);
    attribs_clean_current();
    attribs_mask(0xFFu);
    if (S.layout != (1ull << 63 | stride)) {
        /* ProgVertex: pos 0, d0 16, d1 32, fog 48, t0..t3 52.. */
        static const int off[8] = { 0, 16, 32, 48, 52, 68, 84, 100 };
        static const int cnt[8] = { 4, 4, 4, 1, 4, 4, 4, 4 };
        int a;
        for (a = 0; a < 8; a++)
            glVertexAttribPointer((GLuint)a, cnt[a], GL_FLOAT, GL_FALSE, (GLsizei)stride,
                                  (const void *)(GLintptr)off[a]);
        S.layout = 1ull << 63 | stride;
    }
    PERF_STEP(PZ_DUP);
    if (s_uber_check && !uber && g_ps[i].has_uber && g_ps[i].checked < 2) {
        g_ps[i].checked++;
        uber_check(i, gl_prim(prim), (GLint)(voff / (GLintptr)stride), (GLsizei)nv, raster);
        use_program(prog);
    }
    glDrawArrays(gl_prim(prim), (GLint)(voff / (GLintptr)stride), (GLsizei)nv);
    PERF_STEP(PZ_DDRAW);
    return S_OK;
}

/* ---- vertex programs on the GPU (gles_vsh.c) -------------------------------
 * As d3d8_nv2a.c's path: the title's vertex arrays go up as they are, with
 * the indices, and a generated vertex shader runs the program. Points stay
 * on the CPU (squares need a geometry shader, which ES 3.0 lacks).
 * OT_GL_VSH=0 runs every program on the CPU. */

#define VSH_CACHE 1024
static struct { uint64_t key; char *src; int done; } g_vsh[VSH_CACHE];
static int g_nvsh;
#define PAIR_CACHE 4096
/* state: 1 building (job), 2 ready, 3 failed */
static struct { uint64_t key; GLuint prog; int state; PcJob *job; } g_pair[PAIR_CACHE];
static int g_npair;

static int vsh_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("OT_GL_VSH"); on = !(e && e[0] == '0'); }
    return on;
}

static uint64_t fnv64(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
    return h;
}

/* How an element of this DXGI format reaches GL; 0 if it cannot. */
static int gl_attr_fmt(uint32_t dx, GLint *n, GLenum *type, GLboolean *norm, int *integer)
{
    *norm = GL_FALSE; *integer = 0;
    switch (dx) {
    case DXGI_FORMAT_R32_FLOAT:          *n = 1; *type = GL_FLOAT; return 1;
    case DXGI_FORMAT_R32G32_FLOAT:       *n = 2; *type = GL_FLOAT; return 1;
    case DXGI_FORMAT_R32G32B32_FLOAT:    *n = 3; *type = GL_FLOAT; return 1;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: *n = 4; *type = GL_FLOAT; return 1;
    case DXGI_FORMAT_R8_UNORM:           *n = 1; *type = GL_UNSIGNED_BYTE; *norm = GL_TRUE; return 1;
    case DXGI_FORMAT_R8G8_UNORM:         *n = 2; *type = GL_UNSIGNED_BYTE; *norm = GL_TRUE; return 1;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:     *n = 4; *type = GL_UNSIGNED_BYTE; *norm = GL_TRUE; return 1;
    case DXGI_FORMAT_R16_SNORM:          *n = 1; *type = GL_SHORT; *norm = GL_TRUE; return 1;
    case DXGI_FORMAT_R16G16_SNORM:       *n = 2; *type = GL_SHORT; *norm = GL_TRUE; return 1;
    case DXGI_FORMAT_R16G16B16A16_SNORM: *n = 4; *type = GL_SHORT; *norm = GL_TRUE; return 1;
    case DXGI_FORMAT_R16_SINT:           *n = 1; *type = GL_SHORT; *integer = 1; return 1;
    case DXGI_FORMAT_R16G16_SINT:        *n = 2; *type = GL_SHORT; *integer = 1; return 1;
    case DXGI_FORMAT_R16G16B16A16_SINT:  *n = 4; *type = GL_SHORT; *integer = 1; return 1;
    case DXGI_FORMAT_R32_UINT:           *n = 1; *type = GL_UNSIGNED_INT; *integer = 1; return 1;
    default: return 0;
    }
}

/* An array of stride 0 (one element for all vertices) as the attribute's
 * current value, its array disabled. */
static void attr_const_set(GLuint a, uint32_t dx, const uint8_t *p)
{
    float f[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    GLint n; GLenum t; GLboolean nm; int in, k;
    if (!gl_attr_fmt(dx, &n, &t, &nm, &in)) return;
    for (k = 0; k < n; k++) {
        switch (t) {
        case GL_FLOAT: memcpy(&f[k], p + 4 * k, 4); break;
        case GL_UNSIGNED_BYTE: f[k] = p[k] / 255.0f; break;
        case GL_SHORT: {
            int16_t v; memcpy(&v, p + 2 * k, 2);
            if (in) f[k] = (float)v;
            else { f[k] = v / 32767.0f; if (f[k] < -1.0f) f[k] = -1.0f; }
            break;
        }
        case GL_UNSIGNED_INT: { uint32_t v; memcpy(&v, p, 4); glVertexAttribI4ui(a, v, 0, 0, 1); return; }
        }
    }
    if (in) glVertexAttribI4i(a, (GLint)f[0], (GLint)f[1], (GLint)f[2], (GLint)f[3]);
    else glVertexAttrib4fv(a, f);
}

static void vsh_kinds(const Nv2aVshDraw *d, uint8_t kind[16])
{
    int a;
    for (a = 0; a < 16; a++) {
        if (!(d->inputs & (1u << a))) { kind[a] = 0xFF; continue; }
        kind[a] = d->attr[a].kind;
        if ((kind[a] & 0x0F) != NV2A_VSH_IN_CONST && d->attr[a].dxgi_format == DXGI_FORMAT_B8G8R8A8_UNORM)
            kind[a] |= GLES_VSH_IN_BGRA;
    }
}

/* The vertex shader source of this program and these inputs, made on first
 * use (NULL if it cannot be). */
static const char *vsh_get(const Nv2aVshDraw *d, uint64_t *keyp)
{
    static char src[262144];
    uint8_t kind[16];
    uint64_t key;
    unsigned h, n;
    int len;
    vsh_kinds(d, kind);
    key = fnv64(d->prog_hash, kind, sizeof kind);
    if (d->topology == D3DPT_POINTLIST) key = fnv64(key, "pts", 3);
    if (!key) key = 1;
    *keyp = key;
    h = (unsigned)(key ^ (key >> 31)) & (VSH_CACHE - 1);
    for (n = 0; n < VSH_CACHE; n++, h = (h + 1) & (VSH_CACHE - 1)) {
        if (!g_vsh[h].done) break;
        if (g_vsh[h].key == key) return g_vsh[h].src;
    }
    if (n == VSH_CACHE || g_nvsh >= VSH_CACHE * 3 / 4) return NULL;
    g_nvsh++;
    g_vsh[h].key = key;
    g_vsh[h].done = 1;
    g_vsh[h].src = NULL;
    len = gles_vsh_glsl(d->prog, d->prog_len, d->inputs, kind, d->topology == D3DPT_POINTLIST,
                        src, (int)sizeof src);
    if (len <= 0) return NULL;
    g_vsh[h].src = strdup(src);
    return g_vsh[h].src;
}

static void pair_ready(int h, GLuint p)
{
    GLuint bi;
    g_pair[h].prog = p;
    g_pair[h].state = p ? 2 : 3;
    if (!p) return;
    program_bind_units(p, "PshConsts");
    bi = glGetUniformBlockIndex(p, "VshConsts");
    if (bi != GL_INVALID_INDEX) glUniformBlockBinding(p, bi, 1);
    bi = glGetUniformBlockIndex(p, "VshParams");
    if (bi != GL_INVALID_INDEX) glUniformBlockBinding(p, bi, 2);
    gles_invalidate_state();
}

/* The program of a vertex shader and a combiner shader: from the disk cache,
 * else built on the worker thread (gles_progcache.c). 0 until it is ready --
 * the caller then has the draw done on the CPU. */
static GLuint pair_get(uint64_t vkey, const char *vsrc, int psi)
{
    uint64_t key = vkey * 0x9E3779B97F4A7C15ull ^ g_ps[psi].key;
    unsigned h = (unsigned)(key ^ (key >> 29) ^ (key >> 47)) & (PAIR_CACHE - 1), n;
    GLuint p;
    if (!key) key = 1;
    for (n = 0; n < PAIR_CACHE; n++, h = (h + 1) & (PAIR_CACHE - 1)) {
        if (!g_pair[h].key) break;
        if (g_pair[h].key == key) {
            if (g_pair[h].state == 1 && pc_job_poll(g_pair[h].job, &p)) {
                g_pair[h].job = NULL;
                pair_ready((int)h, p);
            }
            return g_pair[h].state == 2 ? g_pair[h].prog : 0;
        }
    }
    if (n == PAIR_CACHE || g_npair >= PAIR_CACHE * 3 / 4) return 0;
    g_npair++;
    g_pair[h].key = key;
    {
        double t0 = now_ms();
        p = pc_load_cached(vsrc, g_ps[psi].src);
        if (!p && !(g_pair[h].job = pc_link_async(vsrc, g_ps[psi].src, "nv2a vertex program"))) {
            p = pc_link_sync(vsrc, g_ps[psi].src, "nv2a vertex program");
            g_nv_compiles++;
            if (g_perf_on) perf_count(PC_COMPILE, 1);
        }
        g_nv_compile_ms += now_ms() - t0;
    }
    if (g_pair[h].job) { g_pair[h].state = 1; return 0; }
    pair_ready((int)h, p);
    return p;
}

/* XBOX_FIX_POINTS_GPU (default 1, as on Windows): the particles' squares are
 * built by the vertex shader, one instance per point; 0 keeps them on the CPU. */
int d3d8_points_gpu_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_POINTS_GPU"); on = !(e && e[0] == '0'); }
    return on && vsh_on();
}

/* OT_UPLOAD_STATS=1: what the vertex-program draws upload, per frame. */
static int s_ustats = -1;
static struct { double vb, vb_unique, ib, draws, frames; } s_us_acc;
static struct { const uint8_t *base; uint32_t size; } s_us_seen[4096];
static int s_us_nseen;

static void ustats_vb(const uint8_t *base, uint32_t size)
{
    int j;
    s_us_acc.vb += size;
    for (j = 0; j < s_us_nseen; j++)
        if (s_us_seen[j].base == base && s_us_seen[j].size == size) return;
    s_us_acc.vb_unique += size;
    if (s_us_nseen < 4096) { s_us_seen[s_us_nseen].base = base; s_us_seen[s_us_nseen].size = size; s_us_nseen++; }
}

void gles_frame_end(void)
{
    static unsigned frames;
    if ((++frames % 600) == 0) pc_report();
    if (s_ustats <= 0) return;
    s_us_nseen = 0;
    if (++s_us_acc.frames >= 60) {
        double f = s_us_acc.frames;
        fprintf(stderr, "[UPLOAD] per frame: %.0f draws, vertices %.2f MB (%.2f MB distinct ranges), indices %.2f MB\n",
                s_us_acc.draws / f, s_us_acc.vb / f / 1048576.0, s_us_acc.vb_unique / f / 1048576.0,
                s_us_acc.ib / f / 1048576.0);
        memset(&s_us_acc, 0, sizeof s_us_acc);
    }
}

int d3d8_nv2a_vsh_ready(const Nv2aVshDraw *d)
{
    uint64_t key;
    int a;
    if (!vsh_on() || (d->topology == D3DPT_POINTLIST && !d3d8_points_gpu_on())) return 0;
    for (a = 0; a < 16; a++) {
        GLint n; GLenum t; GLboolean nm; int in;
        if (!(d->inputs & (1u << a)) || (d->attr[a].kind & 0x0F) == NV2A_VSH_IN_CONST) continue;
        if (!gl_attr_fmt(d->attr[a].dxgi_format, &n, &t, &nm, &in)) return 0;
    }
    return vsh_get(d, &key) != NULL;
}

int d3d8_nv2a_draw_program_gpu(const Nv2aVshDraw *d)
{
    struct { const uint8_t *base; uint32_t stride, size; GLintptr off; } grp[16];
    struct { float screen[4], fog[4], flags[4], vattr[16][4], point[4], glp[4]; } params;
    int psi = ps_find(d->ps_key), a, g, ngrp = 0;
    int8_t agrp[16];
    uint16_t done = 0;
    unsigned cmask = 0, amask = 0;
    int points = 0;
    static uint8_t *gather[16];
    static size_t gather_cap[16];
    uint32_t bv = 0;
    static uint32_t *ib;
    static uint32_t ib_cap;
    GLenum prim;
    const char *vs;
    GLuint prog;
    GLintptr ioff;
    uint64_t vkey;
    double pt;

    if (psi < 0 || !g_ps[psi].src || !d->nindices || !vsh_on()) return 0;
    if (!ps_poll(psi)) {                /* still building: the CPU path, with the ubershader */
        if (!g_ps[psi].has_uber) return 0;
        pc_note_fallback();
        return 2;
    }
    switch (d->topology) {
    case D3DPT_TRIANGLELIST: prim = GL_TRIANGLES;  break;
    case D3DPT_LINELIST:     prim = GL_LINES;      break;
    case D3DPT_LINESTRIP:    prim = GL_LINE_STRIP; break;
    case D3DPT_POINTLIST:    prim = GL_TRIANGLES;  points = 1; break;
    default: return 0;
    }
    gles_check_thread("nv2a_draw_program_gpu");
    vs = vsh_get(d, &vkey);
    if (!vs) return 2;
    prog = pair_get(vkey, vs, psi);
    if (!prog) { pc_note_fallback(); return 2; }       /* still building: on the CPU */
    pt = g_perf_on ? perf_now() : 0.0;

    /* Attributes interleaved in one array share an upload (d3d8_nv2a.c). */
    memset(agrp, -1, sizeof agrp);
    for (;;) {
        int best = -1;
        for (a = 0; a < 16; a++)
            if ((d->inputs & (1u << a)) && !(done & (1u << a)) &&
                (d->attr[a].kind & 0x0F) != NV2A_VSH_IN_CONST &&
                (best < 0 || d->attr[a].base < d->attr[best].base))
                best = a;
        if (best < 0) break;
        done |= (uint16_t)(1u << best);
        a = best;
        if (!d->attr[a].stride) { cmask |= 1u << a; continue; }
        for (g = 0; g < ngrp; g++)
            if (grp[g].stride == d->attr[a].stride && grp[g].stride &&
                d->attr[a].base >= grp[g].base &&
                (uint32_t)(d->attr[a].base - grp[g].base) < grp[g].stride)
                break;
        if (g == ngrp) {
            grp[g].base = d->attr[a].base;
            grp[g].stride = d->attr[a].stride;
            grp[g].size = 0;
            ngrp++;
        }
        {
            uint32_t off = (uint32_t)(d->attr[a].base - grp[g].base);
            if (off + d->attr[a].bytes > grp[g].size) grp[g].size = off + d->attr[a].bytes;
        }
        agrp[a] = (int8_t)g;
    }

    gles_apply_states(d->raster);
    use_program(prog);
    PERF_STEP(PZ_DSTATE);

    glBindVertexArray(s_vao);
    if (points) {
        /* Instances step through an array without indices: the points'
         * vertices, in draw order, one array per group. */
        uint32_t k;
        for (g = 0; g < ngrp; g++) {
            size_t need = (size_t)d->nindices * grp[g].stride;
            if (need > gather_cap[g]) {
                uint8_t *nb = (uint8_t *)realloc(gather[g], need);
                if (!nb) return 0;
                gather[g] = nb;
                gather_cap[g] = need;
            }
            for (k = 0; k < d->nindices; k++) {
                size_t src = (size_t)d->indices[k] * grp[g].stride;
                size_t len = grp[g].stride;
                if (src >= grp[g].size) { memset(gather[g] + (size_t)k * grp[g].stride, 0, len); continue; }
                if (src + len > grp[g].size) len = grp[g].size - src;
                memcpy(gather[g] + (size_t)k * grp[g].stride, grp[g].base + src, len);
            }
        }
    }
    {
        GLsizeiptr total = 0;
        for (g = 0; g < ngrp; g++)
            total += (points ? (GLsizeiptr)d->nindices * grp[g].stride : grp[g].size) + grp[g].stride;
        stream_reserve(&s_vtx, total);
    }
    /* One array (the usual case): put it at a multiple of its stride and add
     * that vertex number to the indices, so the pointers stay at the
     * buffer's start and the next draw of the same layout sets none. */
    for (g = 0; g < ngrp; g++) {
        if (points)
            grp[g].off = stream_put(&s_vtx, gather[g], (GLsizeiptr)d->nindices * grp[g].stride, 4);
        else
            grp[g].off = stream_put(&s_vtx, grp[g].base, (GLsizeiptr)grp[g].size,
                                    ngrp == 1 ? (GLsizeiptr)grp[g].stride : 4);
        if (grp[g].off < 0) return 0;
    }
    if (s_ustats < 0) s_ustats = getenv("OT_UPLOAD_STATS") != NULL;
    if (s_ustats) {
        for (g = 0; g < ngrp; g++) ustats_vb(grp[g].base, grp[g].size);
        s_us_acc.ib += d->nindices * 4.0;
        s_us_acc.draws++;
    }
    if (ngrp == 1 && !points) { bv = (uint32_t)(grp[0].off / grp[0].stride); grp[0].off = 0; }
    for (a = 0; a < 16; a++) if (agrp[a] >= 0) amask |= 1u << a;
    attribs_mask(amask);
    {
        uint64_t lay = 0;
        if (ngrp == 1 && !points) {
            lay = fnv64(1469598103934665603ull, &grp[0].stride, 4);
            for (a = 0; a < 16; a++)
                if (agrp[a] >= 0) {
                    uint32_t v[3] = { (uint32_t)a, d->attr[a].dxgi_format,
                                      (uint32_t)(d->attr[a].base - grp[0].base) };
                    lay = fnv64(lay, v, sizeof v);
                }
            lay = (lay | 1ull << 62) & ~(1ull << 63);
        }
        if (!lay || lay != S.layout) {
            for (a = 0; a < 16; a++) {
                GLint n; GLenum t; GLboolean nm; int in;
                const void *p;
                g = agrp[a];
                if (g < 0 || !gl_attr_fmt(d->attr[a].dxgi_format, &n, &t, &nm, &in)) continue;
                p = (const void *)(grp[g].off + (GLintptr)(d->attr[a].base - grp[g].base));
                if (in) glVertexAttribIPointer((GLuint)a, n, t, (GLsizei)grp[g].stride, p);
                else    glVertexAttribPointer((GLuint)a, n, t, nm, (GLsizei)grp[g].stride, p);
                if (points) glVertexAttribDivisor((GLuint)a, 1);
            }
            S.layout = lay;
        }
    }
    for (a = 0; a < 16; a++)
        if (cmask & (1u << a)) attr_const_set((GLuint)a, d->attr[a].dxgi_format, d->attr[a].base);
    s_cur_dirty |= cmask;
    if (points) {
        ioff = 0;
    } else if (bv) {
        uint32_t k;
        if (d->nindices > ib_cap) {
            uint32_t *nb = (uint32_t *)realloc(ib, (size_t)d->nindices * 4);
            if (!nb) return 0;
            ib = nb;
            ib_cap = d->nindices;
        }
        for (k = 0; k < d->nindices; k++) ib[k] = d->indices[k] + bv;
        ioff = stream_put(&s_idx, ib, (GLsizeiptr)d->nindices * 4, 4);
    } else
        ioff = stream_put(&s_idx, d->indices, (GLsizeiptr)d->nindices * 4, 4);
    if (ioff < 0) return 0;
    {
        static int dumped = -1;
        static uint64_t seen[256];
        static int nseen;
        if (dumped < 0) dumped = getenv("OT_VSH_DUMP") ? 0 : 1;
        if (!dumped && nseen < 256) {
            uint8_t kind[16];
            uint64_t k;
            int j, s0 = 0;
            vsh_kinds(d, kind);
            k = fnv64(d->prog_hash, kind, sizeof kind);
            for (a = 0; a < 16; a++) k = fnv64(k, &d->attr[a].stride, 4);
            for (j = 0; j < nseen; j++) if (seen[j] == k) break;
            if (j == nseen) {
                seen[nseen++] = k;
                fprintf(stderr, "[VSH-DUMP] prog %016llX len %d n %u:", (unsigned long long)d->prog_hash,
                        d->prog_len, d->nindices);
                for (a = 0; a < 16; a++)
                    if ((d->inputs & (1u << a)) && (d->attr[a].kind & 0x0F) != NV2A_VSH_IN_CONST)
                        fprintf(stderr, " v%d:k%02X/f%u/s%u/o%d", a, d->attr[a].kind, d->attr[a].dxgi_format,
                                d->attr[a].stride, agrp[a] >= 0 ? (int)(d->attr[a].base - grp[agrp[a]].base) : -1);
                fprintf(stderr, "\n");
                (void)s0;
            }
        }
    }
    PERF_STEP(PZ_DUP);

    memset(&params, 0, sizeof params);
    params.screen[0] = d->screen_w;
    params.screen[1] = d->screen_h;
    params.screen[2] = d->clip_max;
    params.screen[3] = d->point_zoom;
    params.fog[0] = (float)d->fog_mode;
    params.fog[1] = d->fog_p0;
    params.fog[2] = d->fog_p1;
    params.flags[0] = d->specular ? 1.0f : 0.0f;
    params.flags[1] = d->spec_alpha ? 1.0f : 0.0f;
    params.point[0] = d->point_params ? 1.0f : 0.0f;
    params.point[1] = d->point_size;
    params.point[2] = d->point_smooth ? 1.0f : 0.0f;
    params.point[3] = d->point_kx;
    params.glp[0] = (!(d->raster & 1) && !s_has_depth_clamp) ? 1.0f : 0.0f;
    if (d->attr_const) memcpy(params.vattr, d->attr_const, sizeof params.vattr);
    stream_reserve(&s_ubo, (GLsizeiptr)(VSHCPU_CONSTANTS * 16 + sizeof params + d->ps_consts_size) +
                           3 * s_ubo_align);
    if (!ubo_bind(1, d->vconst, VSHCPU_CONSTANTS * 16) || !ubo_bind(2, &params, sizeof params) ||
        !ubo_bind(0, d->ps_consts, d->ps_consts_size))
        return 0;
    PERF_STEP(PZ_DCB);

    if (points) {
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)d->nindices);
        for (a = 0; a < 16; a++)
            if (agrp[a] >= 0) glVertexAttribDivisor((GLuint)a, 0);
    } else
        glDrawElements(prim, (GLsizei)d->nindices, GL_UNSIGNED_INT, (const void *)ioff);
    if (getenv("OT_VSH_DUMP")) {
        static int n;
        GLenum e = glGetError();
        if (e && n++ < 10) fprintf(stderr, "[VSH-DUMP] GL error %04X\n", e);
    }
    PERF_STEP(PZ_DDRAW);
    return 1;
}
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
    unsigned m = 1;
    S.layout = 0;
    attribs_clean_current();
    if (fvf & D3DFVF_XYZRHW) {
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, (GLsizei)stride, (const void *)off);
        off += 16;
    } else {
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, (GLsizei)stride, (const void *)off);
        off += 12;
    }
    if (fvf & D3DFVF_NORMAL) off += 12;
    if (fvf & D3DFVF_DIFFUSE) {
        m |= 2;
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, (const void *)off);
        off += 4;
    }
    if (fvf & D3DFVF_SPECULAR) {
        m |= 4;
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, (const void *)off);
        off += 4;
    }
    for (t = 0; t < ntex && t < 4; t++) {
        m |= 1u << (4 + t);
        glVertexAttribPointer((GLuint)(4 + t), 2, GL_FLOAT, GL_FALSE, (GLsizei)stride, (const void *)off);
        off += 8;
    }
    attribs_mask(m);
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
    attribs_mask(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    gles_invalidate_state();
}

/* Reserve stream room up front for a multi-part upload (unused for now). */
void gles_stream_reserve(GLsizeiptr bytes) { stream_reserve(&s_vtx, bytes); }
