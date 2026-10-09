/*
 * OpenGL ES 3 renderer -- the screen-space options over the 3D: ambient
 * occlusion (GTAO) and edge smoothing (SMAA). Both off by default, which is
 * the title's own image; the Android options (port/src/host_sdl.c) and
 * settings.ini (XBOX_AO, XBOX_SMAA / XBOX_SMAA_PRESET) turn them on, the
 * setters below while playing.
 *
 * Where: at the end of the 3D of an image, the FRAME_END pass marker
 * (d3d8_PassPhaseChanged, the translator calls it on this thread; the markers
 * are on whenever the HUD placement or these passes need them). The scene
 * target then holds the 3D and nothing else; the title draws its overlay --
 * mist, lens flare, HUD, menus -- over the result afterwards, so text and HUD
 * are never darkened or smoothed. As d3d8_post.h describes for Windows.
 *
 *   1. Ambient occlusion: GTAO (Jimenez et al. 2016, in the form of Intel's
 *      XeGTAO; the pass of WoodyRE's gtao.c in GLSL ES 3.00). The depth of
 *      the target is blitted into a depth texture (a multisampled one is
 *      resolved by the blit); a half-resolution pass rebuilds view-space
 *      positions and normals from it with the camera of the view
 *      (d3d8_NoteProjection, port/src/aspect.c: the projection the title built
 *      last), integrates the visible arc of SLICES directions x STEPS per side
 *      within RADIUS world units (SSX's units are about centimetres: near
 *      plane 25, a rider ~180 tall), slice rotation and step offset from a
 *      4x4 Bayer tile; a full-resolution 4x4 depth-aware blur multiplies
 *      visibility^POWER into the target (blend ZERO / SRC_COLOR, every sample
 *      of a multisampled one).
 *   2. SMAA 1x (Jimenez et al. 2012, smaa/SMAA.hlsl compiled as GLSL ES 3.00:
 *      gles_smaa_src.h): the target is copied (resolved) into a texture,
 *      colour edge detection, blending weights with AreaTex / SearchTex,
 *      neighbourhood blending back into the target. With MSAA both apply,
 *      SMAA on the resolved image. The image is upside down here (the scene
 *      convention): SMAA smooths the mirror image, which is as valid, the
 *      lookups being indexed by edge shapes, not by screen position.
 *
 * An image without a FRAME_END (loading screens drawn by the second render
 * thread, a HUD setting that needs no markers) gets SMAA on the whole image at
 * the present instead (gles_post_present), and no ambient occlusion; video
 * frames get neither.
 */
#include "gles_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../smaa/AreaTex.h"
#include "../smaa/SearchTex.h"
#include "gles_smaa_src.h"

#define AO_RADIUS 100.0f        /* world units */
#define AO_POWER  1.5f          /* visibility^POWER: a little more contrast than the raw integral */

/* ---- the settings ---------------------------------------------------------- */

static volatile LONG s_smaa = -1;       /* 0 off, 1..4 low .. ultra; -1 = from the environment */
static volatile LONG s_ao = -1;         /* 0 / 1 */

static void config_read(void)
{
    if (s_smaa < 0) {
        const char *e = getenv("XBOX_SMAA"), *p = getenv("XBOX_SMAA_PRESET");
        int v = 0;
        if (e && e[0] == '1') {
            v = 3;
            if (p && !_stricmp(p, "low")) v = 1;
            else if (p && !_stricmp(p, "medium")) v = 2;
            else if (p && !_stricmp(p, "ultra")) v = 4;
        }
        InterlockedExchange(&s_smaa, v);
    }
    if (s_ao < 0) {
        const char *e = getenv("XBOX_AO");
        InterlockedExchange(&s_ao, e && e[0] == '1');
    }
}

void d3d8_SetSmaa(int preset) { config_read(); InterlockedExchange(&s_smaa, (preset >= 1 && preset <= 4) ? preset : 0); }
int  d3d8_GetSmaa(void)       { config_read(); return (int)s_smaa; }
void d3d8_SetAo(int on)       { config_read(); InterlockedExchange(&s_ao, on ? 1 : 0); }
int  d3d8_GetAo(void)         { config_read(); return (int)s_ao; }

/* Windows' post chain toggles: here the chain is these passes. */
void d3d8_SetPostProcess(int on) { (void)on; }
int  d3d8_GetPostProcess(void) { return d3d8_GetSmaa() > 0 || d3d8_GetAo(); }
void d3d8_SetPostSplit(int on) { (void)on; }
int  d3d8_GetPostSplit(void) { return 1; }
/* The phase callback is always registered (main.c): the options can be
 * turned on while playing. */
int  d3d8_post_split_wanted(void) { return 1; }

/* ---- the camera ------------------------------------------------------------ */

static volatile float s_fov, s_zn, s_zf;

void d3d8_NoteProjection(float fov_y, float aspect, float zn, float zf)
{
    static int log = -1;
    static unsigned n;
    if (log < 0) { const char *e = getenv("XBOX_PROJ_LOG"); log = e && e[0] == '1'; }
    n++;
    if (log && (fov_y != s_fov || zn != s_zn || zf != s_zf))
        fprintf(stderr, "[PROJ] %u fov %.4f aspect %.4f near %.4f far %.2f\n", n, fov_y, aspect, zn, zf);
    s_fov = fov_y; s_zn = zn; s_zf = zf;
}

/* ---- GL objects ------------------------------------------------------------ */

static int    s_bad_ao, s_bad_smaa;     /* failed once: stays off */
static GLuint s_vao;
static UINT   s_w, s_h;                 /* size of the targets below */
/* SMAA */
static int    s_preset;                 /* of the programs below */
static GLuint s_pr_edge, s_pr_weight, s_pr_blend, s_tex_area, s_tex_search;
static GLint  u_edge_m, u_weight_m, u_blend_m;
static GLuint s_fb_col, s_tex_col, s_fb_edge, s_tex_edge, s_fb_blend, s_tex_blend, s_fb_out, s_tex_out;
/* GTAO */
static GLuint s_pr_ao, s_pr_mix, s_fb_ds, s_tex_ds, s_fb_ao, s_tex_ao;
static GLint  u_ao_full, u_ao_out, u_ao_z, u_ao_scale, u_ao_radius, u_mix_full, u_mix_half, u_mix_z, u_mix_power, u_mix_debug;

static int    s_done;                   /* this image went through the passes at its FRAME_END */
static unsigned long long s_n_split, s_n_present;

/* A full-screen triangle from gl_VertexID, no vertex data. */
#define FSQ_POS "vec4(float((gl_VertexID << 1) & 2) * 2.0 - 1.0, float(gl_VertexID & 2) * 2.0 - 1.0, 0.0, 1.0)"

static void tex_params(GLenum filter)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* A texture of fmt and its framebuffer. */
static GLuint target(GLuint *tex, GLenum fmt, UINT w, UINT h, GLenum filter)
{
    GLuint fb;
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, fmt, (GLsizei)w, (GLsizei)h);
    tex_params(filter);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, fmt == GL_DEPTH24_STENCIL8 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, *tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "[POST] a %ux%u target (0x%x) is incomplete\n", w, h, (unsigned)fmt);
    return fb;
}

static void targets_free(void)
{
    GLuint fb[7] = { s_fb_col, s_fb_edge, s_fb_blend, s_fb_out, s_fb_ds, s_fb_ao, 0 };
    GLuint tx[7] = { s_tex_col, s_tex_edge, s_tex_blend, s_tex_out, s_tex_ds, s_tex_ao, 0 };
    glDeleteFramebuffers(6, fb);
    glDeleteTextures(6, tx);
    s_fb_col = s_fb_edge = s_fb_blend = s_fb_out = s_fb_ds = s_fb_ao = 0;
    s_tex_col = s_tex_edge = s_tex_blend = s_tex_out = s_tex_ds = s_tex_ao = 0;
    s_w = s_h = 0;
}

/* The targets at the scene's size (made when first needed, again when it changes). */
static void targets(void)
{
    UINT w = g_gl.width, h = g_gl.height;
    if (w == s_w && h == s_h) return;
    targets_free();
    s_w = w; s_h = h;
}

/* ---- SMAA ------------------------------------------------------------------ */

static GLuint smaa_program(const char *preset, const char *vs_main, const char *fs_main)
{
    static const char k_pre[] =
        "#version 300 es\nprecision highp float;\nprecision highp sampler2D;\n"
        "#define SMAA_GLSL_3\n#define SMAA_PRESET_%s\n#define SMAA_RT_METRICS uMetrics\n"
        "#define SMAA_INCLUDE_VS %d\n#define SMAA_INCLUDE_PS %d\nuniform vec4 uMetrics;\n";
    size_t n = sizeof k_pre + 64 + sizeof k_smaa_src;
    char *vs = (char *)malloc(n + strlen(vs_main)), *fs = (char *)malloc(n + strlen(fs_main));
    GLuint p = 0;
    if (vs && fs) {
        int k = snprintf(vs, n, k_pre, preset, 1, 0);
        strcpy(vs + k, k_smaa_src);
        strcat(vs, vs_main);
        k = snprintf(fs, n, k_pre, preset, 0, 1);
        strcpy(fs + k, k_smaa_src);
        strcat(fs, fs_main);
        p = gles_compile_program(vs, fs, "smaa");
    }
    free(vs);
    free(fs);
    return p;
}

#define SMAA_VS_UV "void fsq() { vec4 P = " FSQ_POS "; vUV = P.xy * 0.5 + 0.5; gl_Position = P; }\n"
static const char k_vs_edge[] = "out vec2 vUV; out vec4 vOff[3];\n" SMAA_VS_UV
    "void main() { fsq(); SMAAEdgeDetectionVS(vUV, vOff); }\n";
static const char k_fs_edge[] = "uniform sampler2D tex0; in vec2 vUV; in vec4 vOff[3]; out vec4 o;\n"
    "void main() { o = vec4(SMAAColorEdgeDetectionPS(vUV, vOff, tex0), 0.0, 0.0); }\n";
static const char k_vs_weight[] = "out vec2 vUV; out vec2 vPix; out vec4 vOff[3];\n" SMAA_VS_UV
    "void main() { fsq(); SMAABlendingWeightCalculationVS(vUV, vPix, vOff); }\n";
static const char k_fs_weight[] = "uniform sampler2D tex0, tex1, tex2; in vec2 vUV; in vec2 vPix; in vec4 vOff[3]; out vec4 o;\n"
    "void main() { o = SMAABlendingWeightCalculationPS(vUV, vPix, vOff, tex0, tex1, tex2, vec4(0.0)); }\n";
static const char k_vs_blend[] = "out vec2 vUV; out vec4 vOff;\n" SMAA_VS_UV
    "void main() { fsq(); SMAANeighborhoodBlendingVS(vUV, vOff); }\n";
static const char k_fs_blend[] = "uniform sampler2D tex0, tex1; in vec2 vUV; in vec4 vOff; out vec4 o;\n"
    "void main() { o = vec4(SMAANeighborhoodBlendingPS(vUV, vOff, tex0, tex1).rgb, 1.0); }\n";

static void units(GLuint p, int n)
{
    static const char *const k[3] = { "tex0", "tex1", "tex2" };
    int i;
    glUseProgram(p);
    for (i = 0; i < n; i++) glUniform1i(glGetUniformLocation(p, k[i]), i);
}

static int smaa_ready(int preset)
{
    static const char *const k_names[5] = { "", "LOW", "MEDIUM", "HIGH", "ULTRA" };
    if (s_bad_smaa) return 0;
    if (preset != s_preset) {
        if (s_pr_edge) glDeleteProgram(s_pr_edge);
        if (s_pr_weight) glDeleteProgram(s_pr_weight);
        if (s_pr_blend) glDeleteProgram(s_pr_blend);
        s_pr_edge = smaa_program(k_names[preset], k_vs_edge, k_fs_edge);
        s_pr_weight = smaa_program(k_names[preset], k_vs_weight, k_fs_weight);
        s_pr_blend = smaa_program(k_names[preset], k_vs_blend, k_fs_blend);
        if (!s_pr_edge || !s_pr_weight || !s_pr_blend) {
            fprintf(stderr, "[POST] the SMAA shaders failed: edge smoothing is off\n");
            s_bad_smaa = 1;
            return 0;
        }
        units(s_pr_edge, 1);
        units(s_pr_weight, 3);
        units(s_pr_blend, 2);
        u_edge_m = glGetUniformLocation(s_pr_edge, "uMetrics");
        u_weight_m = glGetUniformLocation(s_pr_weight, "uMetrics");
        u_blend_m = glGetUniformLocation(s_pr_blend, "uMetrics");
        s_preset = preset;
        fprintf(stderr, "[POST] SMAA %s\n", k_names[preset]);
    }
    if (!s_tex_area) {                  /* the lookups, in their own row order (indexed by edge shapes) */
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glGenTextures(1, &s_tex_area);
        glBindTexture(GL_TEXTURE_2D, s_tex_area);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, AREATEX_WIDTH, AREATEX_HEIGHT, 0, GL_RG, GL_UNSIGNED_BYTE, areaTexBytes);
        tex_params(GL_LINEAR);
        glGenTextures(1, &s_tex_search);
        glBindTexture(GL_TEXTURE_2D, s_tex_search);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 0, GL_RED, GL_UNSIGNED_BYTE, searchTexBytes);
        tex_params(GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
    if (!s_fb_col) {
        s_fb_col = target(&s_tex_col, GL_RGBA8, s_w, s_h, GL_LINEAR);
        s_fb_edge = target(&s_tex_edge, GL_RGBA8, s_w, s_h, GL_LINEAR);
        s_fb_blend = target(&s_tex_blend, GL_RGBA8, s_w, s_h, GL_LINEAR);
    }
    return 1;
}

static void tex_unit(int unit, GLuint tex)
{
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler((GLuint)unit, 0);     /* the renderer's sampler objects would override the texture's own */
}

/* SMAA of src (s_w x s_h) into the framebuffer dst. */
static void smaa_run(GLuint src, GLuint dst)
{
    float m[4] = { 1.0f / (float)s_w, 1.0f / (float)s_h, (float)s_w, (float)s_h };
    glViewport(0, 0, (GLsizei)s_w, (GLsizei)s_h);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fb_edge);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(s_pr_edge);
    glUniform4fv(u_edge_m, 1, m);
    tex_unit(0, src);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindFramebuffer(GL_FRAMEBUFFER, s_fb_blend);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(s_pr_weight);
    glUniform4fv(u_weight_m, 1, m);
    tex_unit(0, s_tex_edge);
    tex_unit(1, s_tex_area);
    tex_unit(2, s_tex_search);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindFramebuffer(GL_FRAMEBUFFER, dst);
    glUseProgram(s_pr_blend);
    glUniform4fv(u_blend_m, 1, m);
    tex_unit(0, src);
    tex_unit(1, s_tex_blend);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* ---- GTAO ------------------------------------------------------------------ */

static const char k_vs_fsq[] =
    "#version 300 es\nvoid main() { gl_Position = " FSQ_POS "; }\n";

static const char k_fs_ao[] =
    "#version 300 es\nprecision highp float;\nprecision highp sampler2D;\n"
    "uniform sampler2D tex0;\n"               /* the depth (D3D's z/w: the scene's convention) */
    "uniform vec2 uFull, uOut;\n"             /* the depth's size, this pass's size (half) */
    "uniform vec3 uZ;\n"                      /* zn * zf, zf - zn, zf */
    "uniform vec2 uScale;\n"                  /* projection x / y scale */
    "uniform float uRadius;\n"
    "out vec4 o;\n"
    "const float PI = 3.14159265, HALF_PI = 1.57079633;\n"
    "const int SLICES = 3, STEPS = 6;\n"
    "float lin(float d) { return uZ.x / (uZ.z - d * uZ.y); }\n"
    "float dep(vec2 uv) { return textureLod(tex0, uv, 0.0).r; }\n"
    "vec3 viewPos(vec2 uv, float d) { float L = lin(d); vec2 n = uv * 2.0 - 1.0; return vec3(n.x * L / uScale.x, n.y * L / uScale.y, -L); }\n"
    "vec3 posAt(vec2 uv) { return viewPos(uv, dep(uv)); }\n"
    "float fastAcos(float x) { float r = (-0.156583 * abs(x) + HALF_PI) * sqrt(1.0 - abs(x)); return x >= 0.0 ? r : PI - r; }\n"
    "float bayer2(vec2 v) { return mod(2.0 * v.x + 3.0 * v.y, 4.0); }\n"
    "void main() {\n"
    /* the centre of the top-left depth texel of this pixel's 2x2: the centre
     * of the half-resolution pixel falls on a texel corner, where the nearest
     * texel flips with rounding (stripes along rows and columns) */
    "    vec2 inv = 1.0 / uFull, uv = (floor(gl_FragCoord.xy) * (uFull / uOut) + 0.5) * inv;\n"
    "    float d = dep(uv);\n"
    "    if (d >= 1.0) { o = vec4(1.0); return; }\n"
    "    vec3 P = viewPos(uv, d);\n"
    /* the normal from the neighbour on the same surface per axis (the smaller depth step) */
    "    vec3 l = posAt(uv - vec2(inv.x, 0.0)), r = posAt(uv + vec2(inv.x, 0.0)), b = posAt(uv - vec2(0.0, inv.y)), t = posAt(uv + vec2(0.0, inv.y));\n"
    "    vec3 dx = abs(r.z - P.z) < abs(P.z - l.z) ? r - P : P - l;\n"
    "    vec3 dy = abs(t.z - P.z) < abs(P.z - b.z) ? t - P : P - b;\n"
    "    vec3 V = normalize(-P), N = normalize(cross(dx, dy));\n"
    "    if (dot(N, V) < 0.0) N = -N;\n"
    "    float rpx = min(uRadius * uScale.y * 0.5 * uFull.y / -P.z, 0.25 * uFull.y);\n"   /* the radius in pixels, capped */
    "    if (rpx < 1.0) { o = vec4(1.0); return; }\n"
    "    vec2 c = mod(floor(gl_FragCoord.xy), 4.0);\n"
    "    float bay = 4.0 * bayer2(mod(c, 2.0)) + bayer2(floor(c * 0.5));\n"                /* 0..15, one 4x4 tile */
    "    float nSlice = (bay + 0.5) / 16.0, nStep = (mod(bay * 5.0 + 3.0, 16.0) + 0.5) / 16.0;\n"
    "    float fMul = -1.0 / (0.615 * uRadius), fAdd = (1.0 - 0.615) / 0.615 + 1.0;\n"    /* falloff over the outer 61.5 % */
    "    float vis = 0.0;\n"
    "    for (int s = 0; s < SLICES; s++) {\n"
    "        float phi = (float(s) + nSlice) * (PI / float(SLICES));\n"
    "        vec2 om = vec2(cos(phi), sin(phi));\n"
    "        vec3 dir = vec3(om, 0.0), ortho = dir - dot(dir, V) * V, axis = normalize(cross(ortho, V));\n"
    "        vec3 pN = N - axis * dot(N, axis); float pLen = length(pN);\n"
    "        if (pLen < 1e-4) { vis += 1.0; continue; }\n"
    "        float cosN = clamp(dot(pN, V) / pLen, 0.0, 1.0), n = sign(dot(ortho, pN)) * fastAcos(cosN);\n"
    "        float low0 = cos(n + HALF_PI), low1 = cos(n - HALF_PI), h0 = low0, h1 = low1;\n"
    "        for (int k = 0; k < STEPS; k++) {\n"
    "            float st = (float(k) + nStep) / float(STEPS); st *= st;\n"
    "            vec2 off = om * (st * rpx + 1.0) * inv;\n"
    "            vec3 d0 = posAt(uv + off) - P, d1 = posAt(uv - off) - P;\n"
    "            float l0 = length(d0), l1 = length(d1);\n"
    "            h0 = max(h0, mix(low0, dot(d0, V) / l0, clamp(l0 * fMul + fAdd, 0.0, 1.0)));\n"
    "            h1 = max(h1, mix(low1, dot(d1, V) / l1, clamp(l1 * fMul + fAdd, 0.0, 1.0)));\n"
    "        }\n"
    "        float a1 = n + clamp(fastAcos(h0) - n, -HALF_PI, HALF_PI), a0 = n + clamp(-fastAcos(h1) - n, -HALF_PI, HALF_PI), sn = sin(n);\n"
    "        vis += pLen * ((cosN + 2.0 * a0 * sn - cos(2.0 * a0 - n)) + (cosN + 2.0 * a1 * sn - cos(2.0 * a1 - n))) * 0.25;\n"
    "    }\n"
    "    o = vec4(vec3(clamp(vis / float(SLICES), 0.0, 1.0)), 1.0);\n"
    "}\n";

/* The half-resolution occlusion, blurred (4x4 of its texels, weighted by the
 * depth difference) at full resolution: the factor the target is multiplied by. */
static const char k_fs_mix[] =
    "#version 300 es\nprecision highp float;\nprecision highp sampler2D;\n"
    "uniform sampler2D tex0, tex1;\n"         /* the depth, the occlusion */
    "uniform vec2 uFull, uHalf;\n"
    "uniform vec3 uZ;\n"
    "uniform float uPower;\n"
    "uniform int uDebug;\n"
    "out vec4 o;\n"
    "float lin(float d) { return uZ.x / (uZ.z - d * uZ.y); }\n"
    "float dep(vec2 uv) { return textureLod(tex0, uv, 0.0).r; }\n"
    "void main() {\n"
    "    vec2 uv = gl_FragCoord.xy / uFull, inv = 1.0 / uHalf;\n"
    "    float d = dep(uv);\n"
    "    if (d >= 1.0) { o = vec4(1.0); return; }\n"
    "    float Lc = lin(d), sum = 0.0, wsum = 0.0;\n"
    "    if (uDebug == 2) { o = vec4(fract(Lc / 200.0), fract(Lc / 2000.0), 0.0, 1.0); return; }\n"
    "    for (int j = -2; j < 2; j++) for (int i = -2; i < 2; i++) {\n"
    "        vec2 q = uv + (vec2(float(i), float(j)) + 0.5) * inv;\n"
    "        float w = exp(-abs(lin(dep(q)) - Lc) / (0.04 * Lc));\n"
    "        sum += textureLod(tex1, q, 0.0).r * w; wsum += w;\n"
    "    }\n"
    "    float ao = pow(sum / max(wsum, 1e-4), uPower);\n"
    "    o = vec4(ao, ao, ao, 1.0);\n"
    "}\n";

static int ao_ready(void)
{
    if (s_bad_ao) return 0;
    if (!s_pr_ao) {
        s_pr_ao = gles_compile_program(k_vs_fsq, k_fs_ao, "gtao");
        s_pr_mix = gles_compile_program(k_vs_fsq, k_fs_mix, "gtao_mix");
        if (!s_pr_ao || !s_pr_mix) {
            fprintf(stderr, "[POST] the ambient occlusion shaders failed: it is off\n");
            s_bad_ao = 1;
            return 0;
        }
        units(s_pr_ao, 1);
        units(s_pr_mix, 2);
        u_ao_full = glGetUniformLocation(s_pr_ao, "uFull");
        u_ao_out = glGetUniformLocation(s_pr_ao, "uOut");
        u_ao_z = glGetUniformLocation(s_pr_ao, "uZ");
        u_ao_scale = glGetUniformLocation(s_pr_ao, "uScale");
        u_ao_radius = glGetUniformLocation(s_pr_ao, "uRadius");
        u_mix_full = glGetUniformLocation(s_pr_mix, "uFull");
        u_mix_half = glGetUniformLocation(s_pr_mix, "uHalf");
        u_mix_z = glGetUniformLocation(s_pr_mix, "uZ");
        u_mix_power = glGetUniformLocation(s_pr_mix, "uPower");
        u_mix_debug = glGetUniformLocation(s_pr_mix, "uDebug");
        fprintf(stderr, "[POST] ambient occlusion (GTAO, half resolution)\n");
    }
    if (!s_fb_ds) {
        s_fb_ds = target(&s_tex_ds, GL_DEPTH24_STENCIL8, s_w, s_h, GL_NEAREST);
        s_fb_ao = target(&s_tex_ao, GL_R8, (s_w + 1) / 2, (s_h + 1) / 2, GL_LINEAR);
    }
    return 1;
}

/* GTAO into the framebuffer dst (the draw target), which holds the 3D. */
static void ao_run(GLuint dst)
{
    UINT hw = (s_w + 1) / 2, hh = (s_h + 1) / 2;
    float zn = s_zn, zf = s_zf, fov = s_fov, sy, sx;
    if (!(zn > 0.0f) || !(zf > zn) || !(fov > 0.01f && fov < 3.1f)) return;   /* no camera yet */
    sy = 1.0f / tanf(0.5f * fov);
    sx = sy * (float)s_h / (float)s_w;          /* square pixels: the title draws undistorted */

    /* 1. the depth (resolved when multisampled) */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, dst);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fb_ds);
    glBlitFramebuffer(0, 0, (GLint)s_w, (GLint)s_h, 0, 0, (GLint)s_w, (GLint)s_h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

    /* 2. the occlusion at half resolution */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fb_ao);
    glViewport(0, 0, (GLsizei)hw, (GLsizei)hh);
    glUseProgram(s_pr_ao);
    glUniform2f(u_ao_full, (float)s_w, (float)s_h);
    glUniform2f(u_ao_out, (float)hw, (float)hh);
    glUniform3f(u_ao_z, zn * zf, zf - zn, zf);
    glUniform2f(u_ao_scale, sx, sy);
    glUniform1f(u_ao_radius, AO_RADIUS);
    tex_unit(0, s_tex_ds);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* 3. blurred and multiplied into the target */
    glBindFramebuffer(GL_FRAMEBUFFER, dst);
    glViewport(0, 0, (GLsizei)s_w, (GLsizei)s_h);
    glUseProgram(s_pr_mix);
    glUniform2f(u_mix_full, (float)s_w, (float)s_h);
    glUniform2f(u_mix_half, (float)hw, (float)hh);
    glUniform3f(u_mix_z, zn * zf, zf - zn, zf);
    glUniform1f(u_mix_power, AO_POWER);
    tex_unit(0, s_tex_ds);
    tex_unit(1, s_tex_ao);
    {   /* XBOX_AO_DEBUG (testing): 1 the occlusion alone instead of the picture,
         * 2 the distance from the depth (red: every 200 units, green: 2000) */
        static int dbg = -1;
        if (dbg < 0) { const char *e = getenv("XBOX_AO_DEBUG"); dbg = e ? atoi(e) : 0; }
        glUniform1i(u_mix_debug, dbg);
        if (!dbg) {
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_ZERO, GL_SRC_COLOR);
        }
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
}

/* ---- running them ----------------------------------------------------------- */

/* The state the passes need; the renderer sets its own again afterwards. */
static void state_begin(void)
{
    if (!s_vao) glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

static void state_end(void)
{
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, gles_draw_target());
    gles_invalidate_state();
}

/* FRAME_END: the target holds the 3D of the image being built. */
void d3d8_PassPhaseChanged(int old_phase, int new_phase, unsigned tag)
{
    int smaa, ao;
    GLuint dst;
    (void)old_phase; (void)tag;
    if (new_phase != 5 /* PGRAPH_PHASE_END */ || s_done) return;
    config_read();
    smaa = (int)s_smaa;
    ao = (int)s_ao;
    if (!smaa && !ao) return;
    if (d3d8_GuestFramebufferActive()) return;
    targets();
    dst = gles_draw_target();
    state_begin();
    if (ao && ao_ready()) ao_run(dst);
    if (smaa && smaa_ready(smaa)) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, dst);        /* the 3D, resolved, as SMAA's input */
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fb_col);
        glBlitFramebuffer(0, 0, (GLint)s_w, (GLint)s_h, 0, 0, (GLint)s_w, (GLint)s_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        smaa_run(s_tex_col, dst);
    }
    state_end();
    s_done = 1;
    s_n_split++;
}

/* The present (gles_device.c): the texture to show for scene_tex. An image
 * that did not go through the passes at FRAME_END gets SMAA here, on the
 * whole of it. Starts the next image. */
GLuint gles_post_present(GLuint scene_tex)
{
    GLuint out = scene_tex;
    int smaa;
    config_read();
    smaa = (int)s_smaa;
    if (!s_done && smaa && !d3d8_GuestFramebufferActive()) {
        targets();
        if (smaa_ready(smaa)) {
            if (!s_fb_out) s_fb_out = target(&s_tex_out, GL_RGBA8, s_w, s_h, GL_LINEAR);
            state_begin();
            smaa_run(scene_tex, s_fb_out);
            state_end();
            out = s_tex_out;
            s_n_present++;
        }
    }
    s_done = 0;
    {
        static unsigned n;
        if ((smaa || s_ao) && ++n % 1800u == 0u)
            fprintf(stderr, "[POST] %llu images at the end of the 3D, %llu whole at the present\n", s_n_split, s_n_present);
    }
    return out;
}
