/**
 * The register combiners as one GLSL program that reads them at run time.
 *
 * nv2a_psh.c writes a fragment shader per combiner state; building one costs
 * a phone driver milliseconds. While a new one builds (on the worker thread,
 * gles_progcache.c), its draws use this "ubershader": the same rules as
 * nv2a_psh.c's generated code -- the texture-stage modes, the input and output
 * mappings, every read of a stage before its writes (RGB writes, then alpha),
 * the final combiner, alpha test and window clip -- driven by the state packed
 * into the PshUber block by gles_psh_uber_pack(). Slower per pixel, but it
 * is drawn only for the frames the real program takes to arrive.
 *
 * Where the generator writes code that does not compile (an F register in a
 * general stage, a dot product of a stage before stage 0), there is no real
 * program to match; the ubershader reads zero there.
 */
#include "gles_psh_uber.h"
#include <stdlib.h>
#include <string.h>

const char gles_psh_uber_fs[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "layout(std140) uniform PshConsts {\n"
    "    vec4 c0[9]; vec4 c1[9]; vec4 fog_color; vec4 tex_size[4]; vec4 alpha_ref; vec4 clip_region[8];\n"
    "};\n"
    "layout(std140) uniform PshUber {\n"
    "    uvec4 u_main;      /* combiner_control, other_stage_input, final0, final1 */\n"
    "    uvec4 u_modes;     /* texture-stage mode of each stage */\n"
    "    uvec4 u_rgb_in[2]; uvec4 u_rgb_out[2]; uvec4 u_a_in[2]; uvec4 u_a_out[2];\n"
    "    uvec4 u_flags;     /* x rect | alphakill << 4 | cube << 8 | tex0 on << 12; y alpha test | func << 1;\n"
    "                          z simple; w window clip */\n"
    "};\n"
    "uniform sampler2D tex0, tex1, tex2, tex3;\n"
    "in vec4 v_d0; in vec4 v_d1; in float v_fog;\n"
    "in vec4 v_t0; in vec4 v_t1; in vec4 v_t2; in vec4 v_t3;\n"
    "out vec4 o_color;\n"
    "float sign1(float x) { x *= 255.0; return (x - 128.0) / 127.0; }\n"
    "float sign2(float x) { x *= 255.0; return x >= 128.0 ? (x - 255.5) / 127.5 : (x + 0.5) / 127.5; }\n"
    "float sign3(float x) { x *= 255.0; return x >= 128.0 ? (x - 256.0) / 127.0 : x / 127.0; }\n"
    "vec3 dotmap(uint m, vec4 c) {\n"
    "    switch (int(m & 7u)) {\n"
    "    case 1: return vec3(sign1(c.r), sign1(c.g), sign1(c.b));\n"
    "    case 2: return vec3(sign2(c.r), sign2(c.g), sign2(c.b));\n"
    "    case 3: return vec3(sign3(c.r), sign3(c.g), sign3(c.b));\n"
    "    case 4: {\n"
    "        uint hi = (uint(c.a * 255.0) << 8) | uint(c.r * 255.0);\n"
    "        uint lo = (uint(c.g * 255.0) << 8) | uint(c.b * 255.0);\n"
    "        return vec3(float(hi) / 65535.0, float(lo) / 65535.0, 1.0);\n"
    "    }\n"
    "    default: return c.rgb;\n"
    "    }\n"
    "}\n"
    "vec2 remapCubeTo2D(vec3 t) {\n"
    "    vec3 a = abs(t);\n"
    "    vec2 uv;\n"
    "    if (a.x > a.y && a.x > a.z) uv = (t.x > 0.0 ? vec2(-t.z, t.y) : vec2(t.z, t.y)) / a.x;\n"
    "    else if (a.y > a.x && a.y > a.z) uv = (t.y > 0.0 ? vec2(t.x, -t.z) : vec2(t.x, t.z)) / a.y;\n"
    "    else uv = (t.z > 0.0 ? vec2(t.x, t.y) : vec2(-t.x, t.y)) / a.z;\n"
    "    return uv;\n"
    "}\n"
    /* registers by number: 0 zero, 3 fog, 4 v0, 5 v1, 8..11 t0..t3, 12 r0, 13 r1 */
    "vec4 R[16];\n"
    "vec4 EF;\n"
    "int STG;\n"
    "uint FLG;\n"
    "bool SUMCLAMP, INV_V1, INV_R0;\n"
    "vec4 getreg(uint r) {\n"
    "    if (r == 1u) return c0[((FLG & 0x10u) != 0u || STG == 8) ? STG : 0];\n"
    "    if (r == 2u) return c1[((FLG & 0x100u) != 0u || STG == 8) ? STG : 0];\n"
    "    if (r == 14u) {\n"
    "        vec4 v = INV_V1 ? 1.0 - R[5] : R[5], q = INV_R0 ? 1.0 - R[12] : R[12];\n"
    "        vec4 s = vec4(v.rgb + q.rgb, 0.0);\n"
    "        return SUMCLAMP ? clamp(s, 0.0, 1.0) : s;\n"
    "    }\n"
    "    if (r == 15u) return EF;\n"
    "    if (r == 0u || r == 6u || r == 7u) return vec4(0.0);\n"
    "    return R[r];\n"
    "}\n"
    "vec3 map3(uint m, vec3 x) {\n"
    "    switch (int(m)) {\n"
    "    case 0x00: return max(x, 0.0);\n"
    "    case 0x20: return 1.0 - clamp(x, 0.0, 1.0);\n"
    "    case 0x40: return 2.0 * max(x, 0.0) - 1.0;\n"
    "    case 0x60: return -2.0 * max(x, 0.0) + 1.0;\n"
    "    case 0x80: return max(x, 0.0) - 0.5;\n"
    "    case 0xA0: return -max(x, 0.0) + 0.5;\n"
    "    case 0xC0: return x;\n"
    "    default:   return -x;\n"
    "    }\n"
    "}\n"
    "float map1(uint m, float x) { return map3(m, vec3(x)).x; }\n"
    "vec3 in3(uint v) { vec4 x = getreg(v & 15u); return map3(v & 0xE0u, (v & 16u) != 0u ? x.aaa : x.rgb); }\n"
    "float in1(uint v) { vec4 x = getreg(v & 15u); return map1(v & 0xE0u, (v & 16u) != 0u ? x.a : x.b); }\n"
    "vec3 out3(uint m, vec3 e) {\n"
    "    switch (int(m)) {\n"
    "    case 0x08: return e - 0.5;\n"
    "    case 0x10: return e * 2.0;\n"
    "    case 0x18: return (e - 0.5) * 2.0;\n"
    "    case 0x20: return e * 4.0;\n"
    "    case 0x30: return e / 2.0;\n"
    "    default:   return e;\n"
    "    }\n"
    "}\n"
    "float out1(uint m, float e) { return out3(m, vec3(e)).x; }\n"
    "vec4 samp(int i, vec2 uv) {\n"
    "    if (i == 0) return texture(tex0, uv);\n"
    "    if (i == 1) return texture(tex1, uv);\n"
    "    if (i == 2) return texture(tex2, uv);\n"
    "    return texture(tex3, uv);\n"
    "}\n"
    "vec2 norm2(int i, vec2 c) { return ((u_flags.x >> uint(i)) & 1u) != 0u ? c / tex_size[i].xy : c; }\n"
    "bool samples(uint m) {\n"
    "    return !(m == 0u || m == 4u || m == 5u || m == 8u || m == 10u || m == 17u || m == 18u);\n"
    "}\n"
    "void main() {\n"
    "    vec4 PT[4] = vec4[4](v_t0, v_t1, v_t2, v_t3);\n"
    "    float DT[4] = float[4](0.0, 0.0, 0.0, 0.0);\n"
    "    uint osi = u_main.y;\n"
    "    uint DM[4] = uint[4](0u, osi & 15u, (osi >> 4) & 15u, (osi >> 8) & 15u);\n"
    "    uint IT[4] = uint[4](0u, 0u, (osi >> 16) & 3u, (osi >> 20) & 3u);\n"
    "    vec4 frag;\n"
    "    if (u_flags.w != 0u) {\n"
    "        vec2 wc = gl_FragCoord.xy - 0.5; bool hit = false;\n"
    "        for (int k = 0; k < 8; k++)\n"
    "            if (all(greaterThanEqual(wc, clip_region[k].xy)) && all(lessThan(wc, clip_region[k].zw))) hit = true;\n"
    "        if (u_flags.w == 2u ? hit : !hit) discard;\n"
    "    }\n"
    "    for (int k = 0; k < 16; k++) R[k] = vec4(0.0);\n"
    "    R[4] = clamp(v_d0, 0.0, 1.0); R[5] = clamp(v_d1, 0.0, 1.0);\n"
    "    R[3] = vec4(fog_color.rgb, clamp(v_fog, 0.0, 1.0));\n"
    "    EF = vec4(0.0); STG = 0; FLG = u_main.x >> 8; SUMCLAMP = false; INV_V1 = false; INV_R0 = false;\n"
    /* texture stages, in order: each may read the earlier ones */
    "    for (int i = 0; i < 4; i++) {\n"
    "        uint mode = u_modes[i];\n"
    "        vec4 pT = PT[i], t = vec4(0.0);\n"
    "        vec4 tin = R[8 + int(IT[i])];\n"
    "        float dp = dot(pT.xyz, dotmap(DM[i], tin));\n"
    "        float d1 = DT[max(i - 1, 0)], d2 = DT[max(i - 2, 0)];\n"
    "        bool cube = ((u_flags.x >> uint(8 + i)) & 1u) != 0u;\n"
    "        switch (int(mode)) {\n"
    "        case 0: t = vec4(0.0, 0.0, 0.0, 1.0); break;\n"
    "        case 1: case 2:\n"
    "            if (cube) t = samp(i, remapCubeTo2D(vec3(1.0, pT.y / pT.w, -pT.x / pT.w)));\n"
    "            else t = samp(i, norm2(i, pT.xy) / pT.w);\n"
    "            break;\n"
    "        case 3: t = samp(i, remapCubeTo2D(pT.xyz)); break;\n"
    "        case 4: t = pT; break;\n"
    "        case 5: if (pT.x < 0.0 || pT.y < 0.0 || pT.z < 0.0 || pT.w < 0.0) discard; break;\n"
    "        case 6: case 7: t = samp(i, norm2(i, pT.xy)); break;\n"
    "        case 9: DT[i] = dp; t = samp(i, norm2(i, vec2(d1, dp))); break;\n"
    "        case 10: case 17: DT[i] = dp; break;\n"
    "        case 11: {\n"
    "            int j = (i + 1) & 3;\n"
    "            float dn = dot(PT[j].xyz, dotmap(DM[j], R[8 + int(IT[j])]));\n"
    "            DT[i] = dp; t = samp(i, remapCubeTo2D(vec3(d1, dp, dn)));\n"
    "            break;\n"
    "        }\n"
    "        case 12: {\n"
    "            DT[i] = dp;\n"
    "            vec3 n = vec3(d2, d1, dp);\n"
    "            vec3 e = vec3(PT[max(i - 2, 0)].w, PT[max(i - 1, 0)].w, pT.w);\n"
    "            t = samp(i, remapCubeTo2D(2.0 * n * dot(n, e) / dot(n, n) - e));\n"
    "            break;\n"
    "        }\n"
    "        case 13: case 14: DT[i] = dp; t = samp(i, remapCubeTo2D(vec3(d2, d1, dp))); break;\n"
    "        case 15: t = samp(i, tin.ar); break;\n"
    "        case 16: t = samp(i, tin.gb); break;\n"
    "        default: break;\n"
    "        }\n"
    "        if (samples(mode) && ((u_flags.x >> uint(4 + i)) & 1u) != 0u && t.a == 0.0) discard;\n"
    "        R[8 + i] = t;\n"
    "    }\n"
    "    if (u_flags.z != 0u) {\n"
    "        frag = ((u_flags.x >> 12) & 1u) != 0u ? R[8] * R[4] : R[4];\n"
    "    } else {\n"
    "        R[12] = vec4(0.0, 0.0, 0.0, u_modes.x != 0u ? R[8].a : 1.0);\n"
    "        int nst = min(int(u_main.x & 255u), 8);\n"
    "        for (int s = 0; s < nst; s++) {\n"
    "            uint ri = u_rgb_in[s >> 2][s & 3], ro = u_rgb_out[s >> 2][s & 3];\n"
    "            uint ai = u_a_in[s >> 2][s & 3], ao = u_a_out[s >> 2][s & 3];\n"
    "            STG = s;\n"
    /* every read of the stage first */
    "            vec3 a = in3(ri >> 24), b = in3((ri >> 16) & 255u), c = in3((ri >> 8) & 255u), d = in3(ri & 255u);\n"
    "            uint f = ro >> 12, m = f & 0x38u;\n"
    "            vec3 ab = (f & 2u) != 0u ? vec3(dot(a, b)) : a * b;\n"
    "            vec3 cd = (f & 1u) != 0u ? vec3(dot(c, d)) : c * d;\n"
    "            bool odd = (FLG & 1u) != 0u ? R[12].a >= 0.5 : (uint(R[12].a * 255.0) & 1u) == 1u;\n"
    "            vec3 ms = (f & 4u) == 0u ? ab + cd : (odd ? cd : ab);\n"
    "            vec3 abr = clamp(out3(m, ab), -1.0, 1.0), cdr = clamp(out3(m, cd), -1.0, 1.0);\n"
    "            vec3 msr = clamp(out3(m, ms), -1.0, 1.0);\n"
    "            float aa = in1(ai >> 24), ba = in1((ai >> 16) & 255u), ca = in1((ai >> 8) & 255u), da = in1(ai & 255u);\n"
    "            uint fa = ao >> 12, ma = fa & 0x38u;\n"
    "            float aba = aa * ba, cda = ca * da;\n"
    "            float msa = (fa & 4u) == 0u ? aba + cda : (odd ? cda : aba);\n"
    "            float abra = clamp(out1(ma, aba), -1.0, 1.0), cdra = clamp(out1(ma, cda), -1.0, 1.0);\n"
    "            float msra = clamp(out1(ma, msa), -1.0, 1.0);\n"
    /* then the writes: RGB (with blue to alpha), then alpha */
    "            uint abd = (ro >> 4) & 15u, cdd = ro & 15u, msd = (ro >> 8) & 15u;\n"
    "            if (abd != 0u) { R[abd].rgb = abr; if ((f & 0x80u) != 0u) R[abd].a = abr.b; }\n"
    "            if (cdd != 0u) { R[cdd].rgb = cdr; if ((f & 0x40u) != 0u) R[cdd].a = cdr.b; }\n"
    "            if (msd != 0u) R[msd].rgb = msr;\n"
    "            abd = (ao >> 4) & 15u; cdd = ao & 15u; msd = (ao >> 8) & 15u;\n"
    "            if (abd != 0u) R[abd].a = abra;\n"
    "            if (cdd != 0u) R[cdd].a = cdra;\n"
    "            if (msd != 0u) R[msd].a = msra;\n"
    "        }\n"
    "        uint f0 = u_main.z, f1 = u_main.w;\n"
    "        if (f0 != 0u || f1 != 0u) {\n"
    "            STG = 8;\n"
    "            SUMCLAMP = (f1 & 0x80u) != 0u; INV_V1 = (f1 & 0x40u) != 0u; INV_R0 = (f1 & 0x20u) != 0u;\n"
    "            vec3 e = in3(f1 >> 24), ff = in3((f1 >> 16) & 255u);\n"
    "            EF = vec4(e * ff, 0.0);\n"
    "            vec3 A = in3(f0 >> 24), B = in3((f0 >> 16) & 255u), C = in3((f0 >> 8) & 255u), D = in3(f0 & 255u);\n"
    "            frag.rgb = D + mix(C, B, A);\n"
    "            frag.a = in1((f1 >> 8) & 255u);\n"
    "        } else {\n"
    "            frag = R[12];\n"
    "        }\n"
    "    }\n"
    "    if ((u_flags.y & 1u) != 0u) {\n"
    "        uint fn = (u_flags.y >> 1) & 7u;\n"
    "        int av = int(round(clamp(frag.a, 0.0, 1.0) * 255.0)), rf = int(alpha_ref.x);\n"
    "        bool pass = fn == 1u ? av < rf : fn == 2u ? av == rf : fn == 3u ? av <= rf :\n"
    "                    fn == 4u ? av > rf : fn == 5u ? av != rf : fn == 6u ? av >= rf : fn == 7u;\n"
    "        if (!pass) discard;\n"
    "    }\n"
    "    o_color = frag;\n"
    "}\n";

/* As nv2a_psh_generate decides the stage modes (XBOX_FIX_TEXPASSTHRU). */
static int stage_samples(int m)
{
    return !(m == 0x00 || m == 0x04 || m == 0x05 || m == 0x08 || m == 0x0a || m == 0x11 || m == 0x12);
}

int gles_psh_uber_pack(const Nv2aPshState *st, uint32_t out[GLES_PSH_UBER_WORDS])
{
    static int fix = -1;
    int i;
    uint32_t rect = 0, kill = 0, cube = 0;
    if (st->show) return 0;             /* diagnostic output: the real shader only */
    if (fix < 0) { const char *e = getenv("XBOX_FIX_TEXPASSTHRU"); fix = !(e && e[0] == '0'); }
    memset(out, 0, GLES_PSH_UBER_WORDS * sizeof *out);
    out[0] = st->combiner_control;
    out[1] = st->other_stage_input;
    out[2] = st->final0;
    out[3] = st->final1;
    for (i = 0; i < 4; i++) {
        int m = (int)((st->shader_stage_program >> (i * 5)) & 0x1F);
        if (!st->tex_enabled[i] && (!fix || stage_samples(m))) m = 0;
        if (st->simple) m = i == 0 && st->tex_enabled[0] ? 0x01 : 0;
        out[4 + i] = (uint32_t)m;
        if (st->tex_rect[i]) rect |= 1u << i;
        if (st->alphakill[i]) kill |= 1u << i;
        if (st->tex_cube[i]) cube |= 1u << i;
    }
    for (i = 0; i < 8; i++) {
        out[8 + i] = st->rgb_in[i];
        out[16 + i] = st->rgb_out[i];
        out[24 + i] = st->alpha_in[i];
        out[32 + i] = st->alpha_out[i];
    }
    out[40] = rect | kill << 4 | cube << 8 | (st->tex_enabled[0] ? 1u << 12 : 0);
    out[41] = (st->alpha_test && st->alpha_func != 7) ? 1u | (uint32_t)(st->alpha_func & 7) << 1 : 0;
    out[42] = st->simple ? 1u : 0;
    out[43] = st->window_clip;
    return 1;
}
