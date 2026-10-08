/*
 * OpenGL ES 3 renderer -- textures, vertex and index buffers.
 *
 * Textures keep the D3D8 Lock/Unlock contract of d3d8_resources.c: the title
 * (or the translator) writes level data in the Xbox layout into a CPU copy,
 * and UnlockRect uploads it. What reaches GL is RGBA8, converted here, except
 * DXT blocks, which go up as S3TC when the GPU takes it (desktop GL drivers,
 * some Android GPUs) and are decoded to RGBA8 otherwise (most phones).
 *
 * Vertex and index buffers stay on the CPU: the draws that use them are the
 * fixed-function ones, which stream their vertices anyway (gles_draw.c).
 */
#include "gles_internal.h"
#include "../d3d8_swizzle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

static int is_dxt(D3DFORMAT f)
{
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT3 || f == D3DFMT_DXT5;
}

static UINT bytes_per_texel(D3DFORMAT f)
{
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
    case D3DFMT_LIN_A8R8G8B8: case D3DFMT_LIN_X8R8G8B8:
        return 4;
    case D3DFMT_R5G6B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_LIN_R5G6B5: case D3DFMT_LIN_A1R5G5B5: case D3DFMT_LIN_A4R4G4B4:
    case D3DFMT_A8L8:
        return 2;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8:
        return 1;
    default:
        return 4;
    }
}

static UINT row_pitch(D3DFORMAT f, UINT w)
{
    if (is_dxt(f)) return ((w + 3) / 4) * (f == D3DFMT_DXT1 ? 8u : 16u);
    return w * bytes_per_texel(f);
}

static void level_dims(const GlTexture *t, UINT level, UINT *w, UINT *h, UINT *pitch, UINT *rows)
{
    UINT lw = t->width >> level, lh = t->height >> level;
    if (!lw) lw = 1;
    if (!lh) lh = 1;
    *w = lw; *h = lh;
    *pitch = row_pitch(t->format, lw);
    *rows = is_dxt(t->format) ? (lh + 3) / 4 : lh;
}

int gles_has_s3tc(void)
{
    static int have = -1;
    if (have < 0) {
        const char *e = getenv("XBOX_GLES_S3TC");     /* 0 forces the CPU decode */
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        have = ext && (strstr(ext, "GL_EXT_texture_compression_s3tc") ||
                       strstr(ext, "GL_NV_texture_compression_s3tc")) ? 1 : 0;
        if (e && e[0] == '0') have = 0;
        fprintf(stderr, "[GLES] DXT textures: %s\n", have ? "S3TC on the GPU" : "decoded on the CPU");
    }
    return have;
}

/* ---- DXT (S3TC) decoding ------------------------------------------------ */

static void rgb565(uint16_t c, uint8_t out[3])
{
    out[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
    out[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
    out[2] = (uint8_t)((c & 31) * 255 / 31);
}

/* One 4x4 colour block into `dst` (RGBA8, row stride in bytes); `dxt1`
 * enables the 3-colour + transparent mode. */
static void decode_color_block(const uint8_t *b, uint8_t *dst, size_t stride, int dxt1,
                               int bw, int bh)
{
    uint16_t c0 = (uint16_t)(b[0] | (b[1] << 8)), c1 = (uint16_t)(b[2] | (b[3] << 8));
    uint32_t bits = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    uint8_t pal[4][4];
    int x, y;
    rgb565(c0, pal[0]); pal[0][3] = 255;
    rgb565(c1, pal[1]); pal[1][3] = 255;
    if (c0 > c1 || !dxt1) {
        for (x = 0; x < 3; x++) {
            pal[2][x] = (uint8_t)((2 * pal[0][x] + pal[1][x]) / 3);
            pal[3][x] = (uint8_t)((pal[0][x] + 2 * pal[1][x]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (x = 0; x < 3; x++) pal[2][x] = (uint8_t)((pal[0][x] + pal[1][x]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    for (y = 0; y < bh; y++)
        for (x = 0; x < bw; x++) {
            int i = (int)((bits >> (2 * (y * 4 + x))) & 3);
            memcpy(dst + y * stride + x * 4, pal[i], 4);
        }
}

static void decode_dxt(D3DFORMAT f, const uint8_t *src, UINT w, UINT h, uint8_t *dst)
{
    UINT bx, by, bw = (w + 3) / 4, bh = (h + 3) / 4;
    size_t stride = (size_t)w * 4;
    for (by = 0; by < bh; by++)
        for (bx = 0; bx < bw; bx++) {
            const uint8_t *blk = src + (by * bw + bx) * (f == D3DFMT_DXT1 ? 8 : 16);
            uint8_t *d = dst + (size_t)by * 4 * stride + bx * 16;
            int cw = (int)(w - bx * 4 < 4 ? w - bx * 4 : 4), ch = (int)(h - by * 4 < 4 ? h - by * 4 : 4);
            int x, y;
            if (f == D3DFMT_DXT1) {
                decode_color_block(blk, d, stride, 1, cw, ch);
                continue;
            }
            decode_color_block(blk + 8, d, stride, 0, cw, ch);
            if (f == D3DFMT_DXT3) {
                for (y = 0; y < ch; y++)
                    for (x = 0; x < cw; x++) {
                        int n = (blk[(y * 4 + x) / 2] >> (((y * 4 + x) & 1) * 4)) & 15;
                        d[y * stride + x * 4 + 3] = (uint8_t)(n * 17);
                    }
            } else {
                uint8_t a[8];
                uint64_t ab = 0;
                int i;
                a[0] = blk[0]; a[1] = blk[1];
                if (a[0] > a[1])
                    for (i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1]) / 7);
                else {
                    for (i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1]) / 5);
                    a[6] = 0; a[7] = 255;
                }
                for (i = 0; i < 6; i++) ab |= (uint64_t)blk[2 + i] << (8 * i);
                for (y = 0; y < ch; y++)
                    for (x = 0; x < cw; x++)
                        d[y * stride + x * 4 + 3] = a[(ab >> (3 * (y * 4 + x))) & 7];
            }
        }
}

/* ---- uncompressed formats to RGBA8 -------------------------------------- */

static void texel_to_rgba(D3DFORMAT f, const uint8_t *p, uint8_t *o)
{
    uint16_t v;
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_LIN_A8R8G8B8:
        o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3]; return;
    case D3DFMT_X8R8G8B8: case D3DFMT_LIN_X8R8G8B8:
        o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 255; return;
    case D3DFMT_R5G6B5: case D3DFMT_LIN_R5G6B5:
        v = (uint16_t)(p[0] | (p[1] << 8));
        rgb565(v, o); o[3] = 255; return;
    case D3DFMT_A1R5G5B5: case D3DFMT_LIN_A1R5G5B5:
        v = (uint16_t)(p[0] | (p[1] << 8));
        o[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31); o[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
        o[2] = (uint8_t)((v & 31) * 255 / 31); o[3] = (v & 0x8000) ? 255 : 0; return;
    case D3DFMT_A4R4G4B4: case D3DFMT_LIN_A4R4G4B4:
        v = (uint16_t)(p[0] | (p[1] << 8));
        o[0] = (uint8_t)(((v >> 8) & 15) * 17); o[1] = (uint8_t)(((v >> 4) & 15) * 17);
        o[2] = (uint8_t)((v & 15) * 17); o[3] = (uint8_t)(((v >> 12) & 15) * 17); return;
    case D3DFMT_A8:
        o[0] = o[1] = o[2] = 0; o[3] = p[0]; return;
    case D3DFMT_L8:
        o[0] = o[1] = o[2] = p[0]; o[3] = 255; return;
    case D3DFMT_A8L8:
        o[0] = o[1] = o[2] = p[0]; o[3] = p[1]; return;
    default:
        o[0] = 255; o[1] = 0; o[2] = 255; o[3] = 255; return;   /* undecoded: magenta */
    }
}

static void upload_level(GlTexture *t, UINT level, const BYTE *data)
{
    UINT w, h, pitch, rows, x, y;
    level_dims(t, level, &w, &h, &pitch, &rows);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    if (is_dxt(t->format)) {
        if (t->compressed) {
            glCompressedTexSubImage2D(GL_TEXTURE_2D, (GLint)level, 0, 0, (GLsizei)w, (GLsizei)h,
                t->format == D3DFMT_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT :
                t->format == D3DFMT_DXT3 ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT :
                                           GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,
                (GLsizei)(pitch * rows), data);
        } else {
            uint8_t *rgba = (uint8_t *)malloc((size_t)((w + 3) & ~3u) * ((h + 3) & ~3u) * 4);
            if (!rgba) return;
            /* Decode into a block-aligned scratch, upload the w x h part. */
            decode_dxt(t->format, data, (w + 3) & ~3u, (h + 3) & ~3u, rgba);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)((w + 3) & ~3u));
            glTexSubImage2D(GL_TEXTURE_2D, (GLint)level, 0, 0, (GLsizei)w, (GLsizei)h,
                            GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            free(rgba);
        }
        return;
    }
    {
        UINT bpt = bytes_per_texel(t->format);
        const BYTE *src = data;
        BYTE *lin = NULL;
        uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
        if (!rgba) return;
        if (d3d8_format_is_swizzled(t->format)) {
            lin = (BYTE *)malloc((size_t)w * h * bpt);
            if (lin) { xbox_unswizzle_rect(lin, data, w, h, bpt); src = lin; pitch = w * bpt; }
        }
        if (t->format == D3DFMT_LIN_A8R8G8B8 || t->format == D3DFMT_A8R8G8B8) {
            for (y = 0; y < h; y++) {
                const uint8_t *s = src + (size_t)y * pitch;
                uint8_t *d = rgba + (size_t)y * w * 4;
                for (x = 0; x < w; x++, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; }
            }
        } else {
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++)
                    texel_to_rgba(t->format, src + (size_t)y * pitch + x * bpt,
                                  rgba + ((size_t)y * w + x) * 4);
        }
        glTexSubImage2D(GL_TEXTURE_2D, (GLint)level, 0, 0, (GLsizei)w, (GLsizei)h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        free(rgba);
        free(lin);
    }
}

/* ---- IDirect3DTexture8 --------------------------------------------------- */

static HRESULT __stdcall tex_QueryInterface(IDirect3DTexture8 *s, const IID *r, void **p)
{ (void)s; (void)r; (void)p; return E_NOINTERFACE; }
static ULONG __stdcall tex_AddRef(IDirect3DTexture8 *s)
{ return (ULONG)InterlockedIncrement(&((GlTexture *)s)->ref_count); }
static ULONG __stdcall tex_Release(IDirect3DTexture8 *s)
{
    GlTexture *t = (GlTexture *)s;
    LONG r = InterlockedDecrement(&t->ref_count);
    if (r <= 0) {
        if (t->tex) glDeleteTextures(1, &t->tex);
        free(t->sys_mem);
        free(t->lvl_mem);
        free(t);
    }
    return (ULONG)r;
}
static HRESULT __stdcall tex_GetDevice(IDirect3DTexture8 *s, IDirect3DDevice8 **pp)
{ (void)s; *pp = xbox_GetD3DDevice(); return S_OK; }
static DWORD __stdcall tex_SetPriority(IDirect3DTexture8 *s, DWORD p) { (void)s; (void)p; return 0; }
static DWORD __stdcall tex_GetPriority(IDirect3DTexture8 *s) { (void)s; return 0; }
static void  __stdcall tex_PreLoad(IDirect3DTexture8 *s) { (void)s; }
static DWORD __stdcall tex_GetType(IDirect3DTexture8 *s) { (void)s; return 5; }
static DWORD __stdcall tex_GetLevelCount(IDirect3DTexture8 *s) { return ((GlTexture *)s)->levels; }
static HRESULT __stdcall tex_GetLevelDesc(IDirect3DTexture8 *s, UINT lvl, D3DSURFACE_DESC *d)
{
    GlTexture *t = (GlTexture *)s;
    if (!d || lvl >= t->levels) return E_INVALIDARG;
    memset(d, 0, sizeof *d);
    d->Format = t->format;
    d->Width = t->width >> lvl ? t->width >> lvl : 1;
    d->Height = t->height >> lvl ? t->height >> lvl : 1;
    d->Pool = D3DPOOL_DEFAULT;
    return S_OK;
}
static HRESULT __stdcall tex_GetSurfaceLevel(IDirect3DTexture8 *s, UINT l, IDirect3DSurface8 **pp)
{ (void)s; (void)l; (void)pp; return E_NOTIMPL; }

static HRESULT __stdcall tex_LockRect(IDirect3DTexture8 *s, UINT lvl, D3DLOCKED_RECT *lr,
                                      const RECT *rc, DWORD flags)
{
    GlTexture *t = (GlTexture *)s;
    (void)rc; (void)flags;
    if (!lr || lvl >= t->levels || t->locked) return E_INVALIDARG;
    if (!t->sys_mem) return E_FAIL;              /* a wrapped GPU-only texture */
    if (lvl == 0) {
        lr->Pitch = (INT)t->pitch;
        lr->pBits = t->sys_mem;
    } else {
        UINT w, h, pitch, rows;
        level_dims(t, lvl, &w, &h, &pitch, &rows);
        free(t->lvl_mem);
        t->lvl_mem = (BYTE *)calloc(1, (size_t)pitch * rows);
        if (!t->lvl_mem) return E_OUTOFMEMORY;
        lr->Pitch = (INT)pitch;
        lr->pBits = t->lvl_mem;
    }
    t->lock_level = lvl;
    t->locked = TRUE;
    return S_OK;
}

static HRESULT __stdcall tex_UnlockRect(IDirect3DTexture8 *s, UINT lvl)
{
    GlTexture *t = (GlTexture *)s;
    if (!t->locked || lvl != t->lock_level) return E_FAIL;
    t->locked = FALSE;
    gles_check_thread("UnlockRect");
    upload_level(t, lvl, lvl ? t->lvl_mem : t->sys_mem);
    if (lvl) { free(t->lvl_mem); t->lvl_mem = NULL; }
    return S_OK;
}

static const IDirect3DTexture8Vtbl g_tex_vtbl = {
    tex_QueryInterface, tex_AddRef, tex_Release,
    tex_GetDevice, tex_SetPriority, tex_GetPriority, tex_PreLoad, tex_GetType,
    tex_GetLevelCount, tex_GetLevelDesc, tex_GetSurfaceLevel, tex_LockRect, tex_UnlockRect,
};

HRESULT gles_CreateTexture(UINT w, UINT h, UINT levels, D3DFORMAT fmt, IDirect3DTexture8 **out)
{
    GlTexture *t;
    GLenum ifmt;
    if (!out || !w || !h) return E_INVALIDARG;
    gles_check_thread("CreateTexture");
    t = (GlTexture *)calloc(1, sizeof *t);
    if (!t) return E_OUTOFMEMORY;
    t->iface.lpVtbl = &g_tex_vtbl;
    t->ref_count = 1;
    t->width = w; t->height = h;
    t->levels = levels ? levels : 1;
    {   /* a chain cannot be longer than the halvings of the larger side */
        UINT full = 1, d = w > h ? w : h;
        while (d > 1) { d >>= 1; full++; }
        if (t->levels > full) t->levels = full;
    }
    t->format = fmt;
    t->pitch = row_pitch(fmt, w);
    t->sys_mem = (BYTE *)calloc(1, (size_t)t->pitch * (is_dxt(fmt) ? (h + 3) / 4 : h));
    if (!t->sys_mem) { free(t); return E_OUTOFMEMORY; }
    t->compressed = is_dxt(fmt) && gles_has_s3tc();
    ifmt = !t->compressed ? GL_RGBA8 :
           fmt == D3DFMT_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT :
           fmt == D3DFMT_DXT3 ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexStorage2D(GL_TEXTURE_2D, (GLsizei)t->levels, ifmt, (GLsizei)w, (GLsizei)h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)t->levels - 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "[GLES] texture %ux%u fmt %u (%u levels) could not be created\n",
                w, h, (unsigned)fmt, t->levels);
        glDeleteTextures(1, &t->tex);
        free(t->sys_mem);
        free(t);
        return E_FAIL;
    }
    *out = &t->iface;
    return S_OK;
}

IDirect3DTexture8 *gles_WrapTexture(GLuint tex, UINT w, UINT h)
{
    GlTexture *t = (GlTexture *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->iface.lpVtbl = &g_tex_vtbl;
    t->ref_count = 1;
    t->tex = tex;
    t->width = w; t->height = h; t->levels = 1;
    t->format = D3DFMT_A8R8G8B8;
    return &t->iface;
}

/* ---- vertex / index buffers (CPU copies) -------------------------------- */

static HRESULT __stdcall buf_QueryInterface(void *s, const IID *r, void **p)
{ (void)s; (void)r; (void)p; return E_NOINTERFACE; }
static ULONG __stdcall buf_AddRef(void *s) { return (ULONG)InterlockedIncrement(&((GlBuffer *)s)->ref_count); }
static ULONG __stdcall buf_Release(void *s)
{
    GlBuffer *b = (GlBuffer *)s;
    LONG r = InterlockedDecrement(&b->ref_count);
    if (r <= 0) { free(b->sys_mem); free(b); }
    return (ULONG)r;
}
static HRESULT __stdcall buf_GetDevice(void *s, IDirect3DDevice8 **pp)
{ (void)s; *pp = xbox_GetD3DDevice(); return S_OK; }
static DWORD __stdcall buf_SetPriority(void *s, DWORD p) { (void)s; (void)p; return 0; }
static DWORD __stdcall buf_GetPriority(void *s) { (void)s; return 0; }
static void  __stdcall buf_PreLoad(void *s) { (void)s; }
static DWORD __stdcall vb_GetType(void *s) { (void)s; return 3; }
static DWORD __stdcall ib_GetType(void *s) { (void)s; return 4; }
static HRESULT __stdcall buf_Lock(void *s, UINT off, UINT sz, BYTE **pp, DWORD flags)
{
    GlBuffer *b = (GlBuffer *)s;
    (void)sz; (void)flags;
    if (!pp || off > b->size) return E_INVALIDARG;
    *pp = b->sys_mem + off;
    b->locked = TRUE;
    return S_OK;
}
static HRESULT __stdcall buf_Unlock(void *s) { ((GlBuffer *)s)->locked = FALSE; return S_OK; }
static HRESULT __stdcall buf_GetDesc(void *s, void *d) { (void)s; (void)d; return E_NOTIMPL; }

static const IDirect3DVertexBuffer8Vtbl g_vb_vtbl = {
    (void *)buf_QueryInterface, (void *)buf_AddRef, (void *)buf_Release, (void *)buf_GetDevice,
    (void *)buf_SetPriority, (void *)buf_GetPriority, (void *)buf_PreLoad, (void *)vb_GetType,
    (void *)buf_Lock, (void *)buf_Unlock, (void *)buf_GetDesc,
};
static const IDirect3DIndexBuffer8Vtbl g_ib_vtbl = {
    (void *)buf_QueryInterface, (void *)buf_AddRef, (void *)buf_Release, (void *)buf_GetDevice,
    (void *)buf_SetPriority, (void *)buf_GetPriority, (void *)buf_PreLoad, (void *)ib_GetType,
    (void *)buf_Lock, (void *)buf_Unlock, (void *)buf_GetDesc,
};

static GlBuffer *buf_new(UINT len)
{
    GlBuffer *b = (GlBuffer *)calloc(1, sizeof *b);
    if (!b) return NULL;
    b->sys_mem = (BYTE *)calloc(1, len ? len : 1);
    if (!b->sys_mem) { free(b); return NULL; }
    b->size = len;
    b->ref_count = 1;
    return b;
}

HRESULT gles_CreateVertexBuffer(UINT len, DWORD fvf, IDirect3DVertexBuffer8 **out)
{
    GlBuffer *b;
    if (!out) return E_INVALIDARG;
    if (!(b = buf_new(len))) return E_OUTOFMEMORY;
    b->iface.vb.lpVtbl = &g_vb_vtbl;
    b->fvf = fvf;
    *out = &b->iface.vb;
    return S_OK;
}

HRESULT gles_CreateIndexBuffer(UINT len, D3DFORMAT fmt, IDirect3DIndexBuffer8 **out)
{
    GlBuffer *b;
    if (!out) return E_INVALIDARG;
    if (!(b = buf_new(len))) return E_OUTOFMEMORY;
    b->iface.ib.lpVtbl = &g_ib_vtbl;
    b->format = fmt;
    *out = &b->iface.ib;
    return S_OK;
}
