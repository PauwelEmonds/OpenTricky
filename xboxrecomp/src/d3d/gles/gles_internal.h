/*
 * OpenGL ES 3 renderer -- internal header.
 *
 * The POSIX counterpart of the D3D11 renderer (d3d8_device.c and friends):
 * the same IDirect3DDevice8 vtable and d3d8_* entry points the push-buffer
 * translator (nv2a_pgraph_d3d11.c) calls, implemented on OpenGL ES 3.0 so one
 * build runs on Linux desktops and Android phones.
 *
 * Conventions, chosen so the translator's D3D numbers can be used as they are:
 *  - The scene is rendered into an FBO upside down: clip-space y is negated,
 *    so window coordinates, gl_FragCoord, viewports and scissor rectangles are
 *    numerically the D3D ones (origin top-left). The present flips it back.
 *  - Clip-space z goes from D3D's [0, w] to GL's [-w, w] in every vertex
 *    shader (z' = 2z - w); with glDepthRangef(MinZ, MaxZ) the stored depth is
 *    then exactly D3D's.
 *  - Because window y is not flipped by GL, a triangle D3D calls clockwise is
 *    counter-clockwise for GL: front faces are swapped (gles_draw.c).
 *
 * All GL calls happen on the thread that created the device (the push-buffer
 * pump thread); entry points called from other threads only store values.
 */
#ifndef OT_GLES_INTERNAL_H
#define OT_GLES_INTERNAL_H

#include <windows.h>
#include "../d3d8_xbox.h"
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GLES_MAX_STAGES 4

/* ---- resources (gles_resources.c) ------------------------------------- */

typedef struct GlTexture {
    IDirect3DTexture8 iface;        /* must be first */
    LONG     ref_count;
    GLuint   tex;
    UINT     width, height, levels;
    D3DFORMAT format;
    int      compressed;            /* uploaded as S3TC blocks (else RGBA8) */
    BYTE    *sys_mem;               /* level 0, in the title's layout */
    UINT     pitch;
    BOOL     locked;
    UINT     lock_level;
    BYTE    *lvl_mem;
} GlTexture;

typedef struct GlBuffer {           /* vertex and index buffers (FFP path) */
    union { IDirect3DVertexBuffer8 vb; IDirect3DIndexBuffer8 ib; } iface;
    LONG     ref_count;
    BYTE    *sys_mem;
    UINT     size;
    D3DFORMAT format;               /* index buffers */
    DWORD    fvf;
    BOOL     locked;
} GlBuffer;

HRESULT gles_CreateTexture(UINT w, UINT h, UINT levels, D3DFORMAT fmt, IDirect3DTexture8 **out);
HRESULT gles_CreateVertexBuffer(UINT len, DWORD fvf, IDirect3DVertexBuffer8 **out);
HRESULT gles_CreateIndexBuffer(UINT len, D3DFORMAT fmt, IDirect3DIndexBuffer8 **out);
/* Wrap an existing GL texture (the previous-frame copy) as a D3D texture. */
IDirect3DTexture8 *gles_WrapTexture(GLuint tex, UINT w, UINT h);
int  gles_has_s3tc(void);

/* ---- device state (gles_device.c) -------------------------------------- */

#define GLES_MAX_RS        256
#define GLES_MAX_TSS       32
#define GLES_MAX_TRANSFORM 512
#define GLES_MAX_LIGHTS    8

typedef struct GlDevice {
    void    *window;                /* SDL_Window * */
    void    *context;               /* SDL_GLContext */
    unsigned long render_thread;    /* pthread_self() of the device's creator */

    UINT     width, height;         /* the scene target */
    UINT     guest_w, guest_h;      /* the title's own back buffer (640x480) */
    UINT     msaa, msaa_requested;

    GLuint   scene_fbo, scene_tex, scene_depth;   /* resolved, sampleable */
    GLuint   ms_fbo, ms_color, ms_depth;           /* multisampled, when AA */

    DWORD    rs[GLES_MAX_RS];
    DWORD    tss[GLES_MAX_STAGES][GLES_MAX_TSS];
    D3DMATRIX transforms[GLES_MAX_TRANSFORM];
    D3DVIEWPORT8 viewport;
    D3DMATERIAL8 material;
    D3DLIGHT8 lights[GLES_MAX_LIGHTS];
    BOOL     light_enable[GLES_MAX_LIGHTS];
    DWORD    vertex_shader, pixel_shader;
    IDirect3DBaseTexture8 *textures[GLES_MAX_STAGES];
    IDirect3DVertexBuffer8 *stream0;
    UINT     stream0_stride;
    IDirect3DIndexBuffer8 *indices;
    UINT     base_vertex;
} GlDevice;

extern GlDevice g_gl;

GLuint gles_draw_target(void);      /* the FBO draws go to (ms_fbo or scene_fbo) */
UINT   d3d8_GetBackbufferWidth(void);  /* the title's frame (640x480) */
UINT   d3d8_GetBackbufferHeight(void);
void   gles_check_thread(const char *what);
void   gles_GetGuestScale(float *sx, float *sy);

/* ---- drawing (gles_draw.c) --------------------------------------------- */

int  gles_draw_init(void);
void gles_draw_shutdown(void);
void gles_invalidate_state(void);   /* after anything outside gles_draw.c touched GL state */
void gles_apply_states(int raster); /* raster: d3d8_nv2a_draw bits, or -1 for the D3D8 states */
void gles_set_viewport_from_device(void);
HRESULT gles_draw_ffp(D3DPRIMITIVETYPE prim, UINT prim_count, const void *verts, UINT stride,
                      const void *indices, int index_bytes, UINT nverts);
/* Full-screen textured triangle into the bound framebuffer (present, video). */
void gles_blit(GLuint tex, int x, int y, int w, int h, int flip_v, GLuint lut);
GLuint gles_compile_program(const char *vs, const char *fs, const char *tag);

/* Window clip (d3d8_SetWindowClip) in title pixels. */
extern int  g_gles_wclip_on;
extern long g_gles_wclip[4];

#ifdef __cplusplus
}
#endif

#endif /* OT_GLES_INTERNAL_H */
