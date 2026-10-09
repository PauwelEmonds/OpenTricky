/**
 * NV2A vertex programs as GLSL ES 3.00 vertex shaders: d3d8_nv2a_vsh.c's
 * HLSL generator, written for GL. The rules are the same -- vshcpu_run()'s
 * order of reads and writes, R12 as oPos, 0 * anything = 0, ARL's biased
 * floor, the per-vertex rules of vsh_vertex() -- and the epilogue ends as
 * gles_draw.c's pass-through shader does (y flipped for the scene target,
 * z from D3D's 0..w to GL's -w..w).
 */
#include "gles_internal.h"
#include "gles_vsh.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
       MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
enum { MUX_NONE, MUX_R, MUX_V, MUX_C };

typedef struct { char *p, *end; int overflow; } Sb;

static void sb(Sb *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (b->overflow) return;
    va_start(ap, fmt);
    n = vsnprintf(b->p, (size_t)(b->end - b->p), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= b->end - b->p) { b->overflow = 1; return; }
    b->p += n;
}

static const char *treg(int n)
{
    static const char *names[16] = { "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
                                     "r8", "r9", "r10", "r11", "O0", "r13", "r14", "r15" };
    return names[n & 15];
}

static void mask_str(unsigned mask, char *out)
{
    int k = 0;
    out[k++] = '.';
    if (mask & 8) out[k++] = 'x';
    if (mask & 4) out[k++] = 'y';
    if (mask & 2) out[k++] = 'z';
    if (mask & 1) out[k++] = 'w';
    out[k] = 0;
}

static void src_expr(const vshcpu_insn *i, int k, char *out, size_t n)
{
    static const char comp[4] = { 'x', 'y', 'z', 'w' };
    char base[48], sw[6];
    int j;
    switch (i->src[k].mux) {
    case MUX_R: snprintf(base, sizeof base, "%s", treg(i->src[k].reg)); break;
    case MUX_V: snprintf(base, sizeof base, "v%u", i->vidx & 15); break;
    case MUX_C:
        if (i->a0x) snprintf(base, sizeof base, "cread(%u + a0)", i->cidx);
        else        snprintf(base, sizeof base, "cread(%u)", i->cidx);
        break;
    default: snprintf(base, sizeof base, "vec4(0.0)"); break;
    }
    sw[0] = '.';
    for (j = 0; j < 4; j++) sw[1 + j] = comp[i->src[k].swz[j] & 3];
    sw[5] = 0;
    snprintf(out, n, "%s(%s)%s", i->src[k].neg ? "-" : "", base, sw);
}

static const char g_prelude[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "layout(std140) uniform VshConsts { vec4 c[192]; };\n"
    "layout(std140) uniform VshParams {\n"
    "    vec4 screen;        /* width, height, CLIP_MAX, oPts scale */\n"
    "    vec4 fogp;          /* mode (0 off, 1 linear, 2 linear abs, 3 exp, 4 exp abs, 5 exp2), p0, p1 */\n"
    "    vec4 flags;         /* specular enable, alpha from specular */\n"
    "    vec4 vattr[16];     /* attributes with no array */\n"
    "    vec4 ptparm;        /* size from oPts, fixed size, smooth */\n"
    "    vec4 glp;           /* x: no depth clip, emulated */\n"
    "};\n"
    "out vec4 v_d0; out vec4 v_d1; out float v_fog;\n"
    "out vec4 v_t0; out vec4 v_t1; out vec4 v_t2; out vec4 v_t3;\n"
    "vec4 cread(int n) { return (n >= 0 && n < 192) ? c[n] : vec4(0.0); }\n"
    "vec4 mul0(vec4 a, vec4 b) {\n"
    "    vec4 p = a * b;\n"
    "    return vec4(a.x == 0.0 || b.x == 0.0 ? 0.0 : p.x, a.y == 0.0 || b.y == 0.0 ? 0.0 : p.y,\n"
    "                a.z == 0.0 || b.z == 0.0 ? 0.0 : p.z, a.w == 0.0 || b.w == 0.0 ? 0.0 : p.w);\n"
    "}\n"
    "float clamp_away(float t) {\n"
    "    if (t > 0.0 || (t == 0.0 && floatBitsToUint(t) == 0u)) return clamp(t, 5.421011e-20, 1.8446744e+19);\n"
    "    return clamp(t, -1.8446744e+19, -5.421011e-20);\n"
    "}\n"
    "bool fin(float v) { return !isnan(v) && !isinf(v); }\n"
    "float fogf(float d) {\n"
    "    float f, m = fogp.x;\n"
    "    if (m == 0.0) return 1.0;\n"
    "    if (m == 2.0 || m == 4.0) d = abs(d);\n"
    "    if ((m == 1.0 || m == 3.0) && isinf(d)) d = 0.0;\n"
    "    if (m <= 2.0) f = fogp.y + d * fogp.z - 1.0;\n"
    "    else if (m <= 4.0) f = fogp.y + exp2(d * fogp.z * 16.0) - 1.5;\n"
    "    else f = fogp.y + exp2(-d * d * fogp.z * fogp.z * 32.0) - 1.5;\n"
    "    if (isnan(f)) return 1.0;\n"
    "    return clamp(f, 0.0, 1.0);\n"
    "}\n"
    "vec4 nan1(vec4 v) { return vec4(isnan(v.x) ? 1.0 : v.x, isnan(v.y) ? 1.0 : v.y,\n"
    "                                isnan(v.z) ? 1.0 : v.z, isnan(v.w) ? 1.0 : v.w); }\n";

int gles_vsh_glsl(const vshcpu_insn *p, int n, uint16_t inputs, const uint8_t kind[16],
                  int points, char *buf, int cap)
{
    Sb b = { buf, buf + cap, 0 };
    int k, a;

    sb(&b, "%s", g_prelude);
    if (points) sb(&b, "#define NV_POINTS 1\n");
    for (a = 0; a < 16; a++) {
        if (!(inputs & (1u << a)) || (kind[a] & 0x0F) == NV2A_VSH_IN_CONST) continue;
        switch (kind[a] & 0x0F) {
        case NV2A_VSH_IN_INT: sb(&b, "layout(location = %d) in ivec4 at%d;\n", a, a); break;
        case NV2A_VSH_IN_CMP: sb(&b, "layout(location = %d) in uint at%d;\n", a, a); break;
        default:              sb(&b, "layout(location = %d) in vec4 at%d;\n", a, a); break;
        }
    }

    sb(&b, "void main() {\n");
    for (a = 0; a < 16; a++) {
        if (!(inputs & (1u << a))) {
            sb(&b, "    vec4 v%d = vec4(0.0, 0.0, 0.0, 1.0);\n", a);
            continue;
        }
        switch (kind[a] & 0x0F) {
        case NV2A_VSH_IN_CONST: sb(&b, "    vec4 v%d = vattr[%d];\n", a, a); break;
        case NV2A_VSH_IN_INT:   sb(&b, "    vec4 v%d = vec4(at%d);\n", a, a); break;
        case NV2A_VSH_IN_CMP:
            /* Packed normal: x 11 bits, y 11 bits, z 10 bits, all signed. */
            sb(&b, "    vec4 v%d = vec4(float(int(at%d << 21u) >> 21) / 1023.0,\n"
                   "        float(int(at%d << 10u) >> 21) / 1023.0, float(int(at%d) >> 22) / 511.0, 1.0);\n",
               a, a, a, a);
            break;
        default:
            /* GL has no BGRA vertex format: B8G8R8A8 arrives as RGBA. */
            sb(&b, "    vec4 v%d = at%d%s;\n", a, a, (kind[a] & GLES_VSH_IN_BGRA) ? ".zyxw" : "");
            break;
        }
        if (kind[a] & NV2A_VSH_IN_W1) sb(&b, "    v%d.w = 1.0;\n", a);
    }
    sb(&b, "    vec4 r0 = vec4(0.0), r1 = r0, r2 = r0, r3 = r0, r4 = r0, r5 = r0, r6 = r0, r7 = r0,\n"
           "         r8 = r0, r9 = r0, r10 = r0, r11 = r0, r13 = r0, r14 = r0, r15 = r0;\n"
           "    vec4 O0 = vec4(0.0, 0.0, 0.0, 1.0), O1 = O0, O2 = O0, O3 = O0, O4 = O0, O5 = O0, O6 = O0,\n"
           "         O7 = O0, O8 = O0, O9 = O0, O10 = O0, O11 = O0, O12 = O0, O13 = O0, O14 = O0, O15 = O0;\n"
           "    vec4 A, B, C, m, u;\n"
           "    int a0 = 0, na0;\n");

    for (k = 0; k < n; k++) {
        const vshcpu_insn *i = &p[k];
        int paired = i->mac != MAC_NOP && i->ilu != ILU_NOP;
        char e[96], mk[6];
        if (i->mac == MAC_NOP && i->ilu == ILU_NOP) {
            if (i->final) break;
            continue;
        }
        sb(&b, "    /* %d */ na0 = a0;\n", k);
        src_expr(i, 0, e, sizeof e); sb(&b, "    A = %s;\n", e);
        src_expr(i, 1, e, sizeof e); sb(&b, "    B = %s;\n", e);
        src_expr(i, 2, e, sizeof e); sb(&b, "    C = %s;\n", e);
        switch (i->mac) {
        case MAC_MOV: sb(&b, "    m = A;\n"); break;
        case MAC_MUL: sb(&b, "    m = mul0(A, B);\n"); break;
        case MAC_ADD: sb(&b, "    m = A + C;\n"); break;
        case MAC_MAD: sb(&b, "    m = mul0(A, B) + C;\n"); break;
        case MAC_DP3: sb(&b, "    m = vec4(A.x * B.x + A.y * B.y + A.z * B.z);\n"); break;
        case MAC_DPH: sb(&b, "    m = vec4(A.x * B.x + A.y * B.y + A.z * B.z + B.w);\n"); break;
        case MAC_DP4: sb(&b, "    m = vec4(A.x * B.x + A.y * B.y + A.z * B.z + A.w * B.w);\n"); break;
        case MAC_DST: sb(&b, "    m = vec4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case MAC_MIN: sb(&b, "    m = vec4(A.x < B.x ? A.x : B.x, A.y < B.y ? A.y : B.y, A.z < B.z ? A.z : B.z, A.w < B.w ? A.w : B.w);\n"); break;
        case MAC_MAX: sb(&b, "    m = vec4(A.x > B.x ? A.x : B.x, A.y > B.y ? A.y : B.y, A.z > B.z ? A.z : B.z, A.w > B.w ? A.w : B.w);\n"); break;
        case MAC_SLT: sb(&b, "    m = vec4(A.x < B.x ? 1.0 : 0.0, A.y < B.y ? 1.0 : 0.0, A.z < B.z ? 1.0 : 0.0, A.w < B.w ? 1.0 : 0.0);\n"); break;
        case MAC_SGE: sb(&b, "    m = vec4(A.x >= B.x ? 1.0 : 0.0, A.y >= B.y ? 1.0 : 0.0, A.z >= B.z ? 1.0 : 0.0, A.w >= B.w ? 1.0 : 0.0);\n"); break;
        case MAC_ARL: sb(&b, "    m = vec4(0.0); na0 = int(floor(A.x + 0.001));\n"); break;
        default:      sb(&b, "    m = vec4(0.0);\n"); break;
        }
        switch (i->ilu) {
        case ILU_MOV: sb(&b, "    u = C;\n"); break;
        case ILU_RCP: sb(&b, "    u = vec4(1.0 / C.x);\n"); break;
        case ILU_RCC: sb(&b, "    u = vec4(clamp_away(1.0 / C.x));\n"); break;
        case ILU_RSQ: sb(&b, "    u = vec4(C.x == 0.0 ? uintBitsToFloat(0x7F800000u) : isinf(C.x) ? 0.0 : 1.0 / sqrt(abs(C.x)));\n"); break;
        case ILU_EXP: sb(&b, "    u = vec4(exp2(floor(C.x)), C.x - floor(C.x), exp2(C.x), 1.0);\n"); break;
        case ILU_LOG:
            sb(&b, "    { float x = abs(C.x);\n"
                   "      if (x == 0.0) u = vec4(uintBitsToFloat(0xFF800000u), 1.0, uintBitsToFloat(0xFF800000u), 1.0);\n"
                   "      else { float e = floor(log2(x)); u = vec4(e, x / exp2(e), log2(x), 1.0); } }\n");
            break;
        case ILU_LIT:
            sb(&b, "    { float x = max(C.x, 0.0), y = max(C.y, 0.0), w = clamp(C.w, -(128.0 - 1.0 / 256.0), 128.0 - 1.0 / 256.0);\n"
                   "      u = vec4(1.0, x, x > 0.0 ? exp2(w * log2(y)) : 0.0, 1.0); }\n");
            break;
        default: sb(&b, "    u = vec4(0.0);\n"); break;
        }
        if (i->mac != MAC_NOP && i->mac != MAC_ARL) {
            unsigned mask = i->mac_mask;
            if (paired && i->out_r == 1) mask = 0;
            if (mask) { mask_str(mask, mk); sb(&b, "    %s%s = m%s;\n", treg(i->out_r), mk, mk); }
        }
        if (i->ilu != ILU_NOP && i->ilu_mask) {
            int reg = paired ? 1 : i->out_r;
            mask_str(i->ilu_mask, mk);
            sb(&b, "    %s%s = u%s;\n", treg(reg), mk, mk);
        }
        if (i->o_mask && i->orb) {
            const char *v = NULL;
            if (i->out_mux == 0 && i->mac != MAC_NOP && i->mac != MAC_ARL) v = "m";
            if (i->out_mux == 1 && i->ilu != ILU_NOP) v = "u";
            if (v) {
                int o = i->out_addr & 0xF;
                if (o == 5) {
                    int comp = (i->o_mask & 8) ? 0 : (i->o_mask & 4) ? 1 : (i->o_mask & 2) ? 2 : 3;
                    sb(&b, "    O5.x = %s.%c;\n", v, "xyzw"[comp]);
                } else {
                    mask_str(i->o_mask, mk);
                    sb(&b, "    O%d%s = %s%s;\n", o, mk, v, mk);
                }
            }
        }
        sb(&b, "    a0 = na0;\n");
        if (i->final) break;
    }

    sb(&b,
       "    float x = trunc(O0.x * 16.0) / 16.0, y = trunc(O0.y * 16.0) / 16.0;\n"
       "    float z = screen.z > 0.0 ? O0.z / screen.z : O0.z;\n"
       "    float w = O0.w;\n"
       "    if (w >= 0.0) w = clamp(w, 5.421011e-20, 1.8446744e19); else w = clamp(w, -1.8446744e19, -5.421011e-20);\n"
       "    float rhw = 1.0 / w, wc = 1.0 / rhw;\n"
       "    bool ok = fin(x) && fin(y) && fin(z) && fin(rhw);\n"
       "#ifdef NV_POINTS\n"
       /* The point's corner: triangles 0 1 2 and 2 1 3 of the corners
        * (-h,-h) (+h,-h) (-h,+h) (+h,+h); size as draw_program() takes it. */
       "    const int cc[6] = int[6](0, 1, 2, 2, 1, 3);\n"
       "    int c = cc[gl_VertexID];\n"
       "    float s = ptparm.x != 0.0 ? O6.x * screen.w : ptparm.y;\n"
       "    if (!(s >= 1.0)) s = 1.0;\n"
       "    if (s > 2048.0) s = 2048.0;\n"
       "    float h = s * 0.5;\n"
       "    x += ((c & 1) != 0 ? h : -h) * ptparm.w;\n"
       "    y += (c & 2) != 0 ? h : -h;\n"
       "#endif\n"
       "    vec4 p = vec4((x / screen.x * 2.0 - 1.0) * wc, (1.0 - y / screen.y * 2.0) * wc, z * wc, wc);\n"
       "    if (glp.x != 0.0 && p.w > 0.0) p.z = clamp(p.z, 0.0, p.w);\n"
       "    gl_Position = vec4(p.x, -p.y, 2.0 * p.z - p.w, p.w);\n"
       "#ifdef NV_POINTS\n"
       /* A point that cannot be placed: all six corners at one spot, no area. */
       "    if (!ok) gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n"
       "#else\n"
       "    if (!ok) gl_Position = vec4(uintBitsToFloat(0x7FC00000u));\n"
       "#endif\n"
       "    gl_PointSize = 1.0;\n"
       "    v_d0 = nan1(O3);\n"
       "    v_d1 = nan1(O4);\n"
       "    if (flags.x == 0.0) v_d1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
       "    else if (flags.y == 0.0) v_d1.w = 1.0;\n"
       "    v_fog = fogf(O5.x);\n"
       "    v_t0 = O9; v_t1 = O10; v_t2 = O11; v_t3 = O12;\n"
       "#ifdef NV_POINTS\n"
       "    if (ptparm.z != 0.0) v_t3 = vec4((c & 1) != 0 ? 1.0 : 0.0, (c & 2) != 0 ? 1.0 : 0.0, 1.0, 1.0);\n"
       "#endif\n"
       "}\n");
    return b.overflow ? -1 : (int)(b.p - buf);
}
