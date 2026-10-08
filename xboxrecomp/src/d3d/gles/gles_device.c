/*
 * OpenGL ES 3 renderer -- the device: context, scene target, present,
 * the IDirect3DDevice8 vtable and the d3d8_* entry points (d3d8_device.c's
 * POSIX counterpart; see gles_internal.h for the conventions).
 *
 * The window belongs to the host (port/src/host_sdl.c), which creates it on
 * the main thread and hands it over with d3d8_SetHostWindow. The device --
 * created by the push-buffer pump thread -- makes its GL context there, and
 * every GL call afterwards happens on that thread.
 */
#include "gles_internal.h"
#include "../../kernel/xbox_perf.h"
#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

GlDevice g_gl;
static IDirect3DDevice8 g_device;
static int g_device_ready;

static int s_cfg_w, s_cfg_h, s_cfg_wide, s_cfg_full;
static double s_cfg_aspect;
static float s_point_zoom = 1.0f;
static volatile LONG s_want_msaa = 1;
static volatile LONG s_show_fps;
static volatile LONG s_frames;
static unsigned g_present_seq;
static DWORD g_last_present_tick;
volatile long g_tex_uploads_log = 0;
volatile double g_tex_upload_ms = 0;

/* ---- host window -------------------------------------------------------- */

static SDL_Window *s_window;

void d3d8_SetHostWindow(void *sdl_window) { s_window = (SDL_Window *)sdl_window; }

/* The app is in the background (Android): no surface to draw into. The
 * renderer holds at its next present until the app is back, then makes its
 * context current on the new surface. */
static volatile LONG s_paused, s_resumed;
void d3d8_SetHostPaused(int paused)
{
    if (paused) InterlockedExchange(&s_paused, 1);
    else { InterlockedExchange(&s_resumed, 1); InterlockedExchange(&s_paused, 0); }
}

static void wait_while_paused(void)
{
    if (!s_paused && !s_resumed) return;
    if (s_paused) fprintf(stderr, "[GLES] in the background: rendering paused\n");
    while (s_paused) SDL_Delay(50);
    if (InterlockedExchange(&s_resumed, 0)) {
        SDL_GL_MakeCurrent(s_window, (SDL_GLContext)g_gl.context);
        gles_invalidate_state();
        fprintf(stderr, "[GLES] back in the foreground\n");
    }
}

/* GL attributes the window must be created with (call before SDL_CreateWindow). */
void d3d8_GlesWindowHints(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

void gles_check_thread(const char *what)
{
    static int told;
    if (g_gl.render_thread && (unsigned long)pthread_self() != g_gl.render_thread && told++ < 8)
        fprintf(stderr, "[GLES] %s called off the render thread (GL calls there do nothing)\n", what);
}

/* ---- settings set before the device exists ------------------------------ */

void d3d8_SetHostDisplay(unsigned w, unsigned h, int wide, int full)
{
    s_cfg_w = (int)w; s_cfg_h = (int)h; s_cfg_wide = wide ? 1 : 0; s_cfg_full = full ? 1 : 0;
}
void   d3d8_SetHostAspect(double a) { s_cfg_aspect = a > 1.0 ? a : 0.0; }
double d3d8_HostAspect(void)
{
    if (s_cfg_aspect > 0.0) return s_cfg_aspect;
    return s_cfg_wide ? 16.0 / 9.0 : 4.0 / 3.0;
}
void  d3d8_SetPointZoom(float z) { s_point_zoom = z > 0.0f ? z : 1.0f; }
float d3d8_PointZoom(void)       { return s_point_zoom; }
void d3d8_SetMsaa(int n)         { InterlockedExchange(&s_want_msaa, (n == 2 || n == 4 || n == 8) ? n : 1); }
int  d3d8_GetMsaa(void)          { return (int)s_want_msaa; }
void d3d8_SetShowFps(int on)     { InterlockedExchange(&s_show_fps, on ? 1 : 0); }
int  d3d8_GetShowFps(void)       { return (int)s_show_fps; }
int  d3d8_HostIsWidescreen(void) { return s_cfg_wide; }
int  d3d8_HostIsFullscreen(void)
{
    return s_window && (SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN) ? 1 : 0;
}
void d3d8_HostSetFullscreen(int on)
{
    if (s_window) SDL_SetWindowFullscreen(s_window, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}
void d3d8_HostSetClientSize(unsigned w, unsigned h)
{
    if (s_window && w && h) { SDL_SetWindowFullscreen(s_window, 0); SDL_SetWindowSize(s_window, (int)w, (int)h); }
}
void d3d8_HostExit(void) { fflush(stdout); fflush(stderr); _exit(0); }
void d3d8_HostToast(const wchar_t *t) { (void)t; }

static D3D8HostUiHooks s_ui_hooks;
void d3d8_SetHostUiHooks(const D3D8HostUiHooks *h)
{
    if (h) s_ui_hooks = *h; else memset(&s_ui_hooks, 0, sizeof s_ui_hooks);
}
/* The host forwards key presses here (F11, F12...). */
int d3d8_HostKey(unsigned vk) { return s_ui_hooks.on_key ? s_ui_hooks.on_key(NULL, vk) : 0; }
HWND d3d8_GetHostWindow(void) { return (HWND)s_window; }
void d3d8_PumpMessages(void) {}

/* ---- scene target ------------------------------------------------------- */

void gles_GetGuestScale(float *sx, float *sy)
{
    *sx = (g_gl.guest_w && g_gl.width)  ? (float)g_gl.width  / (float)g_gl.guest_w : 1.0f;
    *sy = (g_gl.guest_h && g_gl.height) ? (float)g_gl.height / (float)g_gl.guest_h : 1.0f;
}
void d3d8_GetGuestScale(float *sx, float *sy) { gles_GetGuestScale(sx, sy); }
UINT d3d8_GetBackbufferWidth(void)  { return g_gl.guest_w ? g_gl.guest_w : g_gl.width; }
UINT d3d8_GetBackbufferHeight(void) { return g_gl.guest_h ? g_gl.guest_h : g_gl.height; }

GLuint gles_draw_target(void) { return g_gl.ms_fbo ? g_gl.ms_fbo : g_gl.scene_fbo; }

static void scene_release(void)
{
    if (g_gl.ms_fbo) glDeleteFramebuffers(1, &g_gl.ms_fbo);
    if (g_gl.ms_color) glDeleteRenderbuffers(1, &g_gl.ms_color);
    if (g_gl.ms_depth) glDeleteRenderbuffers(1, &g_gl.ms_depth);
    if (g_gl.scene_fbo) glDeleteFramebuffers(1, &g_gl.scene_fbo);
    if (g_gl.scene_tex) glDeleteTextures(1, &g_gl.scene_tex);
    if (g_gl.scene_depth) glDeleteRenderbuffers(1, &g_gl.scene_depth);
    g_gl.ms_fbo = g_gl.ms_color = g_gl.ms_depth = 0;
    g_gl.scene_fbo = g_gl.scene_tex = g_gl.scene_depth = 0;
}

static int scene_create(UINT msaa)
{
    GLint max_samples = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    while (msaa > 1 && (GLint)msaa > max_samples) msaa >>= 1;

    glGenTextures(1, &g_gl.scene_tex);
    glBindTexture(GL_TEXTURE_2D, g_gl.scene_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)g_gl.width, (GLsizei)g_gl.height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &g_gl.scene_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_gl.scene_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_gl.scene_tex, 0);
    if (msaa <= 1) {
        glGenRenderbuffers(1, &g_gl.scene_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, g_gl.scene_depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)g_gl.width, (GLsizei)g_gl.height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_gl.scene_depth);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 0;
    if (msaa > 1) {
        glGenRenderbuffers(1, &g_gl.ms_color);
        glBindRenderbuffer(GL_RENDERBUFFER, g_gl.ms_color);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, (GLsizei)msaa, GL_RGBA8,
                                         (GLsizei)g_gl.width, (GLsizei)g_gl.height);
        glGenRenderbuffers(1, &g_gl.ms_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, g_gl.ms_depth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, (GLsizei)msaa, GL_DEPTH24_STENCIL8,
                                         (GLsizei)g_gl.width, (GLsizei)g_gl.height);
        glGenFramebuffers(1, &g_gl.ms_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g_gl.ms_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_gl.ms_color);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_gl.ms_depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, gles_draw_target());
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glClearColor(0, 0, 0, 1);
    glClearDepthf(1.0f);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    gles_invalidate_state();
    g_gl.msaa = msaa;
    return 1;
}

/* Bring scene_tex up to date with a multisampled frame. */
static void scene_resolve(void)
{
    if (!g_gl.ms_fbo) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gl.ms_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gl.scene_fbo);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, (GLint)g_gl.width, (GLint)g_gl.height, 0, 0, (GLint)g_gl.width,
                      (GLint)g_gl.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, gles_draw_target());
    gles_invalidate_state();
}

/* ---- DAC palette (gamma ramp) ------------------------------------------- */

static pthread_mutex_t s_dac_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char s_dac_lut[256][3];
static volatile LONG s_dac_dirty, s_dac_have;
static int s_dac_identity = 1;
static GLuint s_lut_tex;

int d3d8_GammaFixEnabled(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_GAMMA"); on = !(e && e[0] == '0'); }
    return on;
}

void d3d8_SetDacPalette(const unsigned char lut[256][3])
{
    int i, ident = 1;
    pthread_mutex_lock(&s_dac_lock);
    memcpy(s_dac_lut, lut, sizeof s_dac_lut);
    for (i = 0; i < 256; i++)
        if (lut[i][0] != i || lut[i][1] != i || lut[i][2] != i) ident = 0;
    s_dac_identity = ident;
    pthread_mutex_unlock(&s_dac_lock);
    InterlockedExchange(&s_dac_have, 1);
    InterlockedExchange(&s_dac_dirty, 1);
}

static GLuint dac_lut_tex(void)
{
    if (!d3d8_GammaFixEnabled() || !s_dac_have || s_dac_identity) return 0;
    if (!s_lut_tex) {
        glGenTextures(1, &s_lut_tex);
        glBindTexture(GL_TEXTURE_2D, s_lut_tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 256, 1);
        s_dac_dirty = 1;
    }
    if (InterlockedExchange(&s_dac_dirty, 0)) {
        unsigned char rgba[256 * 4];
        int i;
        pthread_mutex_lock(&s_dac_lock);
        for (i = 0; i < 256; i++) {
            rgba[i * 4 + 0] = s_dac_lut[i][0]; rgba[i * 4 + 1] = s_dac_lut[i][1];
            rgba[i * 4 + 2] = s_dac_lut[i][2]; rgba[i * 4 + 3] = 255;
        }
        pthread_mutex_unlock(&s_dac_lock);
        glBindTexture(GL_TEXTURE_2D, s_lut_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    return s_lut_tex;
}

/* ---- screenshots (uncompressed PNG, as d3d8_device.c's fallback) -------- */

static pthread_mutex_t s_shot_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_shot_path[1024];

void d3d8_RequestScreenshotUtf8(const char *path)
{
    pthread_mutex_lock(&s_shot_lock);
    snprintf(s_shot_path, sizeof s_shot_path, "%s", path ? path : "");
    pthread_mutex_unlock(&s_shot_lock);
}
void d3d8_RequestScreenshot(const wchar_t *path)
{
    char p[1024];
    size_t i;
    for (i = 0; path && path[i] && i < sizeof p - 1; i++) p[i] = (char)(path[i] < 128 ? path[i] : '_');
    p[i] = 0;
    d3d8_RequestScreenshotUtf8(p);
}

/* Where screenshots go; the host may set it (Android: the app's files). */
static char s_shot_dir[1024] = "Screenshots";
void d3d8_SetScreenshotDir(const char *dir) { if (dir) snprintf(s_shot_dir, sizeof s_shot_dir, "%s", dir); }
const char *d3d8_ScreenshotDir(void) { return s_shot_dir; }

static unsigned long png_crc(unsigned long c, const unsigned char *p, size_t n)
{
    static unsigned long t[256];
    size_t i;
    if (!t[1]) {
        unsigned long k, v;
        for (k = 0; k < 256; k++) {
            v = k;
            for (i = 0; i < 8; i++) v = (v & 1) ? 0xEDB88320UL ^ (v >> 1) : v >> 1;
            t[k] = v;
        }
    }
    c ^= 0xFFFFFFFFUL;
    for (i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}
static unsigned char *png_be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
    return p + 4;
}
static unsigned char *png_chunk_end(unsigned char *p, const char *type, size_t len)
{
    png_be32(p, (unsigned long)len);
    memcpy(p + 4, type, 4);
    return png_be32(p + 8 + len, png_crc(0, p + 4, len + 4));
}

/* `rgb`: top-down RGB rows of `stride` bytes. */
static int png_write(const char *path, const unsigned char *rgb, unsigned w, unsigned h, size_t stride)
{
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    const size_t row = (size_t)w * 3 + 1, raw = row * h;
    const size_t blocks = (raw + 65534) / 65535;
    const size_t zlen = 2 + raw + blocks * 5 + 4;
    unsigned char *png, *p, *z;
    size_t left = raw, y, x, done = 0;
    unsigned long a = 1, b = 0;
    char tmp[1100];
    FILE *f;
    int ok;
    png = (unsigned char *)malloc(8 + 25 + 12 + zlen + 12);
    if (!png) return 0;
    memcpy(png, sig, 8);
    p = png + 8;
    png_be32(p + 8, w); png_be32(p + 12, h);
    p[16] = 8; p[17] = 2; p[18] = 0; p[19] = 0; p[20] = 0;
    p = png_chunk_end(p, "IHDR", 13);
    z = p + 8;
    *z++ = 0x78; *z++ = 0x01;
    for (y = 0; y < h; y++) {
        const unsigned char *src = rgb + y * stride;
        for (x = 0; x < row; x++, done++) {
            unsigned char v = x == 0 ? 0 : src[x - 1];
            if (done % 65535 == 0) {
                size_t n = left < 65535 ? left : 65535;
                *z++ = (unsigned char)(left == n);
                *z++ = (unsigned char)n; *z++ = (unsigned char)(n >> 8);
                *z++ = (unsigned char)~n; *z++ = (unsigned char)(~n >> 8);
                left -= n;
            }
            *z++ = v;
            a = (a + v) % 65521; b = (b + a) % 65521;
        }
    }
    z = png_be32(z, (b << 16) | a);
    p = png_chunk_end(p, "IDAT", (size_t)(z - (p + 8)));
    p = png_chunk_end(p, "IEND", 0);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    f = fopen(tmp, "wb");
    ok = f && fwrite(png, 1, (size_t)(p - png), f) == (size_t)(p - png);
    if (f) fclose(f);
    free(png);
    if (ok) ok = rename(tmp, path) == 0;
    return ok;
}

/* The resolved scene, read back as top-down RGB (through the DAC table when
 * it is in use, so a screenshot shows what the window shows). */
static void save_screenshot(const char *path)
{
    size_t n = (size_t)g_gl.width * g_gl.height;
    unsigned char *rgba = (unsigned char *)malloc(n * 4), *rgb = (unsigned char *)malloc(n * 3);
    size_t i;
    int ok = 0;
    if (rgba && rgb) {
        int lut = d3d8_GammaFixEnabled() && s_dac_have && !s_dac_identity;
        unsigned char tab[256][3];
        if (lut) { pthread_mutex_lock(&s_dac_lock); memcpy(tab, s_dac_lut, sizeof tab); pthread_mutex_unlock(&s_dac_lock); }
        glBindFramebuffer(GL_FRAMEBUFFER, g_gl.scene_fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, (GLsizei)g_gl.width, (GLsizei)g_gl.height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        /* The scene is stored top row first already (upside down for GL). */
        for (i = 0; i < n; i++) {
            unsigned char r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
            if (lut) { r = tab[r][0]; g = tab[g][1]; b = tab[b][2]; }
            rgb[i * 3] = r; rgb[i * 3 + 1] = g; rgb[i * 3 + 2] = b;
        }
        ok = png_write(path, rgb, g_gl.width, g_gl.height, (size_t)g_gl.width * 3);
    }
    free(rgba); free(rgb);
    fprintf(stderr, "[GLES] screenshot %s: %s\n", path, ok ? "saved" : "failed");
    glBindFramebuffer(GL_FRAMEBUFFER, gles_draw_target());
    gles_invalidate_state();
}

/* ---- previous frame (the chrome "Master" outfits reflect it) ------------ */

static GLuint s_prev_tex;
static IDirect3DTexture8 *s_prev_wrap;
static unsigned s_prev_wanted;

static void prev_frame_update(void)
{
    if (!s_prev_wanted || g_present_seq - s_prev_wanted > 120) return;
    if (!s_prev_tex) {
        glGenTextures(1, &s_prev_tex);
        glBindTexture(GL_TEXTURE_2D, s_prev_tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)g_gl.width, (GLsizei)g_gl.height);
        s_prev_wrap = gles_WrapTexture(s_prev_tex, g_gl.width, g_gl.height);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gl.scene_fbo);
    glBindTexture(GL_TEXTURE_2D, s_prev_tex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, (GLsizei)g_gl.width, (GLsizei)g_gl.height);
    glBindFramebuffer(GL_FRAMEBUFFER, gles_draw_target());
    gles_invalidate_state();
}

IDirect3DTexture8 *d3d8_PrevFrameTexture(void)
{
    s_prev_wanted = g_present_seq ? g_present_seq : 1;
    return s_prev_wrap;
}

/* ---- present ------------------------------------------------------------ */

static void fps_title(void)
{
    static DWORD t0;
    static LONG f0;
    DWORD now = GetTickCount();
    if (!t0) { t0 = now; f0 = s_frames; return; }
    if (now - t0 >= 1000) {
        if (s_show_fps && s_window) {
            char title[64];
            snprintf(title, sizeof title, "SSX Tricky  -  %u fps",
                     (unsigned)((s_frames - f0) * 1000u / (now - t0)));
            SDL_SetWindowTitle(s_window, title);
        }
        if (getenv("XBOX_FPS_LOG"))
            fprintf(stderr, "[FPS] %.1f\n", (s_frames - f0) * 1000.0 / (now - t0));
        t0 = now; f0 = s_frames;
    }
}

/* Drawn over the shown image, after the game (the host's touch controls). */
static void (*s_overlay)(int w, int h);
void d3d8_SetPresentOverlay(void (*fn)(int w, int h)) { s_overlay = fn; }

static void host_present_impl(void);

/* XBOX_PERF=1: the port's frame profile by zones (kernel/xbox_perf.c). */
void gles_frame_end(void);

static void host_present(void)
{
    double t0;
    gles_frame_end();
    if (!g_perf_on) { host_present_impl(); return; }
    t0 = perf_now();
    host_present_impl();
    perf_add(PZ_PRESENT, perf_now() - t0);
    perf_present_done(-1.0);
}

static void host_present_impl(void)
{
    int dw = 0, dh = 0;
    char shot[1024];
    if (!g_device_ready) return;
    scene_resolve();
    prev_frame_update();

    pthread_mutex_lock(&s_shot_lock);
    snprintf(shot, sizeof shot, "%s", s_shot_path);
    s_shot_path[0] = 0;
    pthread_mutex_unlock(&s_shot_lock);
    if (shot[0]) save_screenshot(shot);
    {   /* OT_SHOT_DIR=<dir> [OT_SHOT_EVERY=n] (testing): a PNG of every n-th
         * present, so a run can be checked without anyone watching it. */
        static const char *dir = (const char *)-1;
        static unsigned every;
        if (dir == (const char *)-1) {
            const char *e = getenv("OT_SHOT_EVERY");
            dir = getenv("OT_SHOT_DIR");
            every = (e && atoi(e) > 0) ? (unsigned)atoi(e) : 120u;
        }
        if (dir && g_present_seq && g_present_seq % every == 0) {
            char p[1100];
            snprintf(p, sizeof p, "%s/f%06u.png", dir, g_present_seq);
            save_screenshot(p);
        }
    }

    SDL_GL_GetDrawableSize(s_window, &dw, &dh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (dw > 0 && dh > 0) {
        const double aspect = d3d8_HostAspect();
        int rw, rh;
        if ((double)dw / (double)dh > aspect) { rh = dh; rw = (int)(dh * aspect + 0.5); }
        else { rw = dw; rh = (int)(dw / aspect + 0.5); }
        if (rw > dw) rw = dw;
        if (rh > dh) rh = dh;
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        gles_blit(g_gl.scene_tex, (dw - rw) / 2, (dh - rh) / 2, rw, rh, 1, dac_lut_tex());
        if (s_overlay) { s_overlay(dw, dh); gles_invalidate_state(); }
    }
    wait_while_paused();
    {
        double ts = g_perf_on ? perf_now() : 0.0;
        SDL_GL_SwapWindow(s_window);
        if (g_perf_on) perf_add(PZ_DXGI, perf_now() - ts);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, gles_draw_target());
    gles_invalidate_state();
    InterlockedIncrement(&s_frames);
    fps_title();

    /* Anti-aliasing changed in the settings: rebuild the targets between frames. */
    if ((UINT)s_want_msaa != g_gl.msaa_requested) {
        g_gl.msaa_requested = (UINT)s_want_msaa;
        scene_release();
        if (!scene_create(g_gl.msaa_requested)) { scene_release(); scene_create(1); }
        if (s_prev_wrap) { s_prev_wrap->lpVtbl->Release(s_prev_wrap); s_prev_wrap = NULL; s_prev_tex = 0; }
        fprintf(stderr, "[GLES] anti-aliasing now %ux\n", g_gl.msaa);
    }
    {   /* translator counter for its pump optimisations (d3d8_nv2a.c on Windows) */
        extern void d3d8_pump_frame_tick(void);
        d3d8_pump_frame_tick();
    }
}

/* ---- guest framebuffer (the video player) ------------------------------- */

static const void *g_fb_src, *g_fb_alt;
static unsigned g_fb_pitch, g_fb_w, g_fb_h;
#define GUEST_FB_IDLE_PRESENTS 30u
static unsigned g_fb_lock_seq, g_fb_locks;

void d3d8_SetGuestFramebuffer(const void *src, unsigned pitch, unsigned w, unsigned h)
{ g_fb_src = src; g_fb_pitch = pitch; g_fb_w = w; g_fb_h = h; }
void d3d8_SetGuestFramebufferAlt(const void *alt) { g_fb_alt = alt; }
void d3d8_NoteGuestFramebufferLock(void) { g_fb_lock_seq = g_present_seq; g_fb_locks++; }
int  d3d8_HasGuestFramebuffer(void) { return g_fb_src != NULL; }
int  d3d8_GuestFramebufferActive(void)
{
    return g_fb_src && g_fb_locks && g_present_seq - g_fb_lock_seq <= GUEST_FB_IDLE_PRESENTS;
}

static unsigned fb_sig(const void *base, unsigned pitch, unsigned h)
{
    const unsigned char *p = (const unsigned char *)base;
    unsigned sig = 2166136261u, y, k;
    if (!p) return 0;
    for (y = 0; y < h; y += 16)
        for (k = 0; k < 64; k += 4) { sig ^= p[(size_t)y * pitch + pitch / 2 + k]; sig *= 16777619u; }
    return sig;
}

/* The newest buffer that is not being written right now, never going back
 * to an older frame (d3d8_device.c's guest_fb_freshest). */
static const void *fb_freshest(void)
{
    static unsigned sig_a, sig_b, gen_a, gen_b, now, shown;
    static int seeded;
    unsigned a, b, cand;
    const void *pick;
    if (!g_fb_alt) return g_fb_src;
    a = fb_sig(g_fb_src, g_fb_pitch, g_fb_h);
    b = fb_sig(g_fb_alt, g_fb_pitch, g_fb_h);
    if (!seeded) { seeded = 1; sig_a = a; sig_b = b; return g_fb_src; }
    now++;
    if (a != sig_a) gen_a = now;
    if (b != sig_b) gen_b = now;
    sig_a = a; sig_b = b;
    if (gen_a == now && gen_b == now) { pick = NULL; cand = 0; }
    else if (gen_a == now) { pick = g_fb_alt; cand = gen_b; }
    else if (gen_b == now) { pick = g_fb_src; cand = gen_a; }
    else if (gen_a >= gen_b) { pick = g_fb_src; cand = gen_a; }
    else { pick = g_fb_alt; cand = gen_b; }
    if (pick && cand < shown) pick = NULL;
    else if (pick) shown = cand;
    return pick;
}

static GLuint s_video_tex;
static unsigned s_video_w, s_video_h;

void d3d8_PresentGuestFramebuffer(const void *src, unsigned pitch, unsigned w, unsigned h)
{
    if (!g_device_ready || !w || !h) return;
    if (!src && !s_video_tex) return;
    if (s_video_tex && (s_video_w != w || s_video_h != h)) { glDeleteTextures(1, &s_video_tex); s_video_tex = 0; }
    if (!s_video_tex) {
        glGenTextures(1, &s_video_tex);
        glBindTexture(GL_TEXTURE_2D, s_video_tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)w, (GLsizei)h);
        s_video_w = w; s_video_h = h;
    }
    if (src) {
        unsigned char *rgba = (unsigned char *)malloc((size_t)w * h * 4);
        unsigned x, y;
        if (!rgba) return;
        for (y = 0; y < h; y++) {
            const unsigned char *s = (const unsigned char *)src + (size_t)y * pitch;
            unsigned char *d = rgba + (size_t)y * w * 4;
            for (x = 0; x < w; x++, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; }
        }
        glBindTexture(GL_TEXTURE_2D, s_video_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        free(rgba);
    }
    /* Into the scene target, scaled to it; the scene is stored top row first. */
    glBindFramebuffer(GL_FRAMEBUFFER, gles_draw_target());
    gles_blit(s_video_tex, 0, 0, (int)g_gl.width, (int)g_gl.height, 0, 0);
}

/* ---- the frame boundary -------------------------------------------------- */

unsigned d3d8_PresentSeq(void) { return g_present_seq; }

DWORD d3d8_MsSincePresent(void)
{
    DWORD now = GetTickCount();
    if (!g_last_present_tick) g_last_present_tick = now;
    return now - g_last_present_tick;
}

void d3d8_PresentFrame(void)
{
    g_present_seq++;
    if (g_fb_src && d3d8_GuestFramebufferActive()) {
        const void *fb = fb_freshest();
        d3d8_PresentGuestFramebuffer(fb, g_fb_pitch, g_fb_w, g_fb_h);
    }
    g_last_present_tick = GetTickCount();
    host_present();
}

/* ---- clears ---------------------------------------------------------------- */

static void clear_target(DWORD flags, D3DCOLOR color, float z, DWORD stencil, int scissor,
                         int x, int y, int w, int h)
{
    GLbitfield bits = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, gles_draw_target());
    if (scissor) { glEnable(GL_SCISSOR_TEST); glScissor(x, y, w, h); }
    else glDisable(GL_SCISSOR_TEST);
    if (flags & D3DCLEAR_TARGET) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f,
                     (color & 0xFF) / 255.0f, ((color >> 24) & 0xFF) / 255.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & D3DCLEAR_ZBUFFER) { glDepthMask(GL_TRUE); glClearDepthf(z); bits |= GL_DEPTH_BUFFER_BIT; }
    if (flags & D3DCLEAR_STENCIL) { glStencilMask(0xFF); glClearStencil((GLint)(stencil & 0xFF)); bits |= GL_STENCIL_BUFFER_BIT; }
    if (bits) glClear(bits);
    gles_invalidate_state();
}

/* The colour clear is limited to the title's clear rectangle (d3d8_device.c
 * explains why: the race intro clears one-pixel strips); depth and stencil
 * are cleared whole, as the D3D11 renderer does. */
void d3d8_ClearRect(DWORD flags, D3DCOLOR color, float z, DWORD stencil,
                    unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    UINT gw = d3d8_GetBackbufferWidth(), gh = d3d8_GetBackbufferHeight();
    if (!g_device_ready) return;
    gles_check_thread("ClearRect");
    if ((flags & D3DCLEAR_TARGET) && !(x0 == 0 && y0 == 0 && x1 + 1 >= gw && y1 + 1 >= gh)) {
        float sx, sy;
        int l, t, r, b;
        gles_GetGuestScale(&sx, &sy);
        if (x1 >= gw) x1 = gw - 1;
        if (y1 >= gh) y1 = gh - 1;
        l = (int)(x0 * sx + 0.5f); t = (int)(y0 * sy + 0.5f);
        r = (int)((x1 + 1) * sx + 0.5f); b = (int)((y1 + 1) * sy + 0.5f);
        if (x0 <= x1 && y0 <= y1) clear_target(D3DCLEAR_TARGET, color, z, stencil, 1, l, t, r - l, b - t);
        flags &= ~(DWORD)D3DCLEAR_TARGET;
    }
    if (flags) clear_target(flags, color, z, stencil, 0, 0, 0, 0, 0);
}

/* ---- occlusion queries (the NV2A zpass counter) ---------------------------
 * GL ES 3 has only "any samples passed"; the translator uses the count to
 * fade lens flares, so 0 or "the whole span" is a fair stand-in: a visible
 * span reports the scene pixels its draws covered at most, approximated by
 * a large number the translator scales down. */
#define OCC_POOL 256
static GLuint s_occ[OCC_POOL];
static unsigned char s_occ_busy[OCC_POOL];

int d3d8_OcclusionBegin(void)
{
    int i;
    if (!g_device_ready) return -1;
    for (i = 0; i < OCC_POOL; i++) if (!s_occ_busy[i]) break;
    if (i == OCC_POOL) return -1;
    if (!s_occ[i]) glGenQueries(1, &s_occ[i]);
    s_occ_busy[i] = 1;
    glBeginQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE, s_occ[i]);
    return i;
}
void d3d8_OcclusionEnd(int h)
{
    if (h < 0 || h >= OCC_POOL || !s_occ[h]) return;
    glEndQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE);
}
int d3d8_OcclusionPoll(int h, unsigned long long *samples)
{
    GLuint avail = 0, any = 0;
    if (h < 0 || h >= OCC_POOL || !s_occ[h]) return -1;
    glGetQueryObjectuiv(s_occ[h], GL_QUERY_RESULT_AVAILABLE, &avail);
    if (!avail) return 0;
    glGetQueryObjectuiv(s_occ[h], GL_QUERY_RESULT, &any);
    *samples = any ? (unsigned long long)g_gl.width * g_gl.height : 0;
    return 1;
}
void  d3d8_OcclusionRelease(int h) { if (h >= 0 && h < OCC_POOL) s_occ_busy[h] = 0; }
float d3d8_OcclusionScale(void)
{
    float sx, sy;
    gles_GetGuestScale(&sx, &sy);
    return (float)(g_gl.msaa ? g_gl.msaa : 1) * sx * sy;
}

/* ---- diagnostics the translator may call --------------------------------- */

unsigned d3d8_DebugPeekScene(float fx, float fy)
{
    unsigned char p[4] = { 0, 0, 0, 0 };
    int x = (int)(fx * g_gl.width), y = (int)(fy * g_gl.height);
    if (!g_device_ready) return 0xDEADBEEFu;
    scene_resolve();
    glBindFramebuffer(GL_FRAMEBUFFER, g_gl.scene_fbo);
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    glBindFramebuffer(GL_FRAMEBUFFER, gles_draw_target());
    gles_invalidate_state();
    return ((unsigned)p[3] << 24) | ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
}
void d3d8_DebugSampleBackbuffer(const char *tag) { (void)tag; }
unsigned d3d8_BackbufferHash(void) { return 0; }
void d3d8_DumpBackbuffer(const char *path) { (void)path; }

/* ---- window clip ------------------------------------------------------------ */

void d3d8_SetWindowClip(int on, long x0, long y0, long x1, long y1)
{
    g_gles_wclip_on = on;
    g_gles_wclip[0] = x0; g_gles_wclip[1] = y0; g_gles_wclip[2] = x1; g_gles_wclip[3] = y1;
}
int d3d8_WindowClipOn(void) { return g_gles_wclip_on; }

/* ======================================================================== */
/* IDirect3DDevice8                                                          */
/* ======================================================================== */

#define SELF (void)self

static HRESULT __stdcall dev_QueryInterface(IDirect3DDevice8 *self, const IID *r, void **p) { SELF; (void)r; (void)p; return E_NOINTERFACE; }
static ULONG __stdcall dev_AddRef(IDirect3DDevice8 *self) { SELF; return 1; }
static ULONG __stdcall dev_Release(IDirect3DDevice8 *self) { SELF; return 1; }
static HRESULT __stdcall dev_GetDirect3D(IDirect3DDevice8 *self, IDirect3D8 **pp) { SELF; (void)pp; return E_NOTIMPL; }
static HRESULT __stdcall dev_GetDeviceCaps(IDirect3DDevice8 *self, void *c) { SELF; (void)c; return S_OK; }
static HRESULT __stdcall dev_GetDisplayMode(IDirect3DDevice8 *self, void *m) { SELF; (void)m; return S_OK; }
static HRESULT __stdcall dev_GetCreationParameters(IDirect3DDevice8 *self, void *p) { SELF; (void)p; return S_OK; }
static HRESULT __stdcall dev_Reset(IDirect3DDevice8 *self, D3DPRESENT_PARAMETERS *pp) { SELF; (void)pp; return S_OK; }
static HRESULT __stdcall dev_Present(IDirect3DDevice8 *self, const RECT *s, const RECT *d, HWND w, void *dirty)
{ SELF; (void)s; (void)d; (void)w; (void)dirty; host_present(); return S_OK; }
static HRESULT __stdcall dev_GetBackBuffer(IDirect3DDevice8 *self, INT i, DWORD t, IDirect3DSurface8 **pp)
{ SELF; (void)i; (void)t; (void)pp; return E_NOTIMPL; }
static HRESULT __stdcall dev_BeginScene(IDirect3DDevice8 *self) { SELF; return S_OK; }
static HRESULT __stdcall dev_EndScene(IDirect3DDevice8 *self) { SELF; return S_OK; }
static HRESULT __stdcall dev_Clear(IDirect3DDevice8 *self, DWORD n, const D3DRECT *r, DWORD flags,
                                   D3DCOLOR c, float z, DWORD s)
{ SELF; (void)n; (void)r; clear_target(flags, c, z, s, 0, 0, 0, 0, 0); return S_OK; }
static HRESULT __stdcall dev_SetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE st, const D3DMATRIX *m)
{ SELF; if ((DWORD)st < GLES_MAX_TRANSFORM && m) g_gl.transforms[(DWORD)st] = *m; return S_OK; }
static HRESULT __stdcall dev_GetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE st, D3DMATRIX *m)
{ SELF; if ((DWORD)st < GLES_MAX_TRANSFORM && m) *m = g_gl.transforms[(DWORD)st]; return S_OK; }
static HRESULT __stdcall dev_SetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE st, DWORD v)
{ SELF; if ((DWORD)st < GLES_MAX_RS) g_gl.rs[(DWORD)st] = v; return S_OK; }
static HRESULT __stdcall dev_GetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE st, DWORD *v)
{ SELF; if ((DWORD)st < GLES_MAX_RS && v) *v = g_gl.rs[(DWORD)st]; return S_OK; }
static HRESULT __stdcall dev_SetTextureStageState(IDirect3DDevice8 *self, DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD v)
{ SELF; if (s < GLES_MAX_STAGES && (DWORD)t < GLES_MAX_TSS) g_gl.tss[s][(DWORD)t] = v; return S_OK; }
static HRESULT __stdcall dev_GetTextureStageState(IDirect3DDevice8 *self, DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD *v)
{ SELF; if (s < GLES_MAX_STAGES && (DWORD)t < GLES_MAX_TSS && v) *v = g_gl.tss[s][(DWORD)t]; return S_OK; }
static HRESULT __stdcall dev_SetTexture(IDirect3DDevice8 *self, DWORD s, IDirect3DBaseTexture8 *t)
{
    SELF;
    if (s >= GLES_MAX_STAGES) return E_INVALIDARG;
    g_gl.textures[s] = t;
    /* As the D3D11 renderer: a bound texture enables its stage, none disables it. */
    if (t) { if (g_gl.tss[s][D3DTSS_COLOROP] == D3DTOP_DISABLE) g_gl.tss[s][D3DTSS_COLOROP] = D3DTOP_MODULATE; }
    else g_gl.tss[s][D3DTSS_COLOROP] = D3DTOP_DISABLE;
    return S_OK;
}
static HRESULT __stdcall dev_GetTexture(IDirect3DDevice8 *self, DWORD s, IDirect3DBaseTexture8 **pp)
{ SELF; if (s >= GLES_MAX_STAGES || !pp) return E_INVALIDARG; *pp = g_gl.textures[s]; return S_OK; }
static HRESULT __stdcall dev_SetStreamSource(IDirect3DDevice8 *self, UINT n, IDirect3DVertexBuffer8 *vb, UINT stride)
{ SELF; if (n == 0) { g_gl.stream0 = vb; g_gl.stream0_stride = stride; } return S_OK; }
static HRESULT __stdcall dev_GetStreamSource(IDirect3DDevice8 *self, UINT n, IDirect3DVertexBuffer8 **pp, UINT *s)
{ SELF; (void)n; if (pp) *pp = g_gl.stream0; if (s) *s = g_gl.stream0_stride; return S_OK; }
static HRESULT __stdcall dev_SetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 *ib, UINT base)
{ SELF; g_gl.indices = ib; g_gl.base_vertex = base; return S_OK; }
static HRESULT __stdcall dev_GetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 **pp, UINT *b)
{ SELF; if (pp) *pp = g_gl.indices; if (b) *b = g_gl.base_vertex; return S_OK; }

static UINT nverts_of(D3DPRIMITIVETYPE p, UINT n)
{
    switch (p) {
    case D3DPT_POINTLIST: return n;
    case D3DPT_LINELIST: return n * 2;
    case D3DPT_LINESTRIP: return n + 1;
    case D3DPT_TRIANGLELIST: return n * 3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return n + 2;
    case D3DPT_QUADLIST: return n * 4;
    default: return 0;
    }
}

static HRESULT __stdcall dev_DrawPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE p, UINT start, UINT n)
{
    GlBuffer *vb = (GlBuffer *)g_gl.stream0;
    UINT stride = g_gl.stream0_stride;
    SELF;
    if (!vb || !stride) return E_FAIL;
    if ((start + nverts_of(p, n)) * stride > vb->size) return E_INVALIDARG;
    return gles_draw_ffp(p, n, vb->sys_mem + start * stride, stride, NULL, 0, 0);
}
static HRESULT __stdcall dev_DrawIndexedPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE p, UINT minv,
                                                  UINT nv, UINT start, UINT n)
{
    GlBuffer *vb = (GlBuffer *)g_gl.stream0, *ib = (GlBuffer *)g_gl.indices;
    UINT stride = g_gl.stream0_stride, isz;
    SELF; (void)minv;
    if (!vb || !ib || !stride) return E_FAIL;
    isz = ib->format == D3DFMT_INDEX32 ? 4 : 2;
    return gles_draw_ffp(p, n, vb->sys_mem + g_gl.base_vertex * stride, stride,
                         ib->sys_mem + start * isz, (int)isz,
                         nv ? minv + nv : (vb->size / stride) - g_gl.base_vertex);
}
static HRESULT __stdcall dev_DrawPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE p, UINT n,
                                             const void *v, UINT stride)
{ SELF; return gles_draw_ffp(p, n, v, stride, NULL, 0, 0); }
static HRESULT __stdcall dev_DrawIndexedPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE p, UINT minv,
        UINT nv, UINT n, const void *idx, D3DFORMAT ifmt, const void *v, UINT stride)
{ SELF; (void)minv; return gles_draw_ffp(p, n, v, stride, idx, ifmt == D3DFMT_INDEX32 ? 4 : 2, nv); }
static HRESULT __stdcall dev_CreateTexture(IDirect3DDevice8 *self, UINT w, UINT h, UINT l, DWORD u,
                                           D3DFORMAT f, D3DPOOL pool, IDirect3DTexture8 **pp)
{ SELF; (void)u; (void)pool; return gles_CreateTexture(w, h, l, f, pp); }
static HRESULT __stdcall dev_CreateVertexBuffer(IDirect3DDevice8 *self, UINT len, DWORD u, DWORD fvf,
                                                D3DPOOL pool, IDirect3DVertexBuffer8 **pp)
{ SELF; (void)u; (void)pool; return gles_CreateVertexBuffer(len, fvf, pp); }
static HRESULT __stdcall dev_CreateIndexBuffer(IDirect3DDevice8 *self, UINT len, DWORD u, D3DFORMAT f,
                                               D3DPOOL pool, IDirect3DIndexBuffer8 **pp)
{ SELF; (void)u; (void)pool; return gles_CreateIndexBuffer(len, f, pp); }
static HRESULT __stdcall dev_CreateRenderTarget(IDirect3DDevice8 *self, UINT w, UINT h, D3DFORMAT f,
        D3DMULTISAMPLE_TYPE m, BOOL l, IDirect3DSurface8 **pp)
{ SELF; (void)w; (void)h; (void)f; (void)m; (void)l; (void)pp; return E_NOTIMPL; }
static HRESULT __stdcall dev_CreateDepthStencilSurface(IDirect3DDevice8 *self, UINT w, UINT h, D3DFORMAT f,
        D3DMULTISAMPLE_TYPE m, IDirect3DSurface8 **pp)
{ SELF; (void)w; (void)h; (void)f; (void)m; (void)pp; return E_NOTIMPL; }
static HRESULT __stdcall dev_SetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 *rt, IDirect3DSurface8 *z)
{ SELF; (void)rt; (void)z; return S_OK; }
static HRESULT __stdcall dev_GetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 **pp) { SELF; (void)pp; return E_NOTIMPL; }
static HRESULT __stdcall dev_GetDepthStencilSurface(IDirect3DDevice8 *self, IDirect3DSurface8 **pp) { SELF; (void)pp; return E_NOTIMPL; }
static HRESULT __stdcall dev_SetViewport(IDirect3DDevice8 *self, const D3DVIEWPORT8 *v)
{ SELF; if (v) g_gl.viewport = *v; return S_OK; }
static HRESULT __stdcall dev_GetViewport(IDirect3DDevice8 *self, D3DVIEWPORT8 *v)
{ SELF; if (v) *v = g_gl.viewport; return S_OK; }
static HRESULT __stdcall dev_SetMaterial(IDirect3DDevice8 *self, const D3DMATERIAL8 *m) { SELF; if (m) g_gl.material = *m; return S_OK; }
static HRESULT __stdcall dev_GetMaterial(IDirect3DDevice8 *self, D3DMATERIAL8 *m) { SELF; if (m) *m = g_gl.material; return S_OK; }
static HRESULT __stdcall dev_SetLight(IDirect3DDevice8 *self, DWORD i, const D3DLIGHT8 *l)
{ SELF; if (i < GLES_MAX_LIGHTS && l) g_gl.lights[i] = *l; return S_OK; }
static HRESULT __stdcall dev_GetLight(IDirect3DDevice8 *self, DWORD i, D3DLIGHT8 *l)
{ SELF; if (i < GLES_MAX_LIGHTS && l) *l = g_gl.lights[i]; return S_OK; }
static HRESULT __stdcall dev_LightEnable(IDirect3DDevice8 *self, DWORD i, BOOL on)
{ SELF; if (i < GLES_MAX_LIGHTS) g_gl.light_enable[i] = on; return S_OK; }
static HRESULT __stdcall dev_SetVertexShader(IDirect3DDevice8 *self, DWORD h) { SELF; g_gl.vertex_shader = h; return S_OK; }
static HRESULT __stdcall dev_GetVertexShader(IDirect3DDevice8 *self, DWORD *h) { SELF; if (h) *h = g_gl.vertex_shader; return S_OK; }
static HRESULT __stdcall dev_SetVertexShaderConstant(IDirect3DDevice8 *self, INT r, const void *d, DWORD n)
{ SELF; (void)r; (void)d; (void)n; return S_OK; }
static HRESULT __stdcall dev_SetPixelShader(IDirect3DDevice8 *self, DWORD h) { SELF; g_gl.pixel_shader = h; return S_OK; }
static HRESULT __stdcall dev_GetPixelShader(IDirect3DDevice8 *self, DWORD *h) { SELF; if (h) *h = g_gl.pixel_shader; return S_OK; }
static HRESULT __stdcall dev_SetPixelShaderConstant(IDirect3DDevice8 *self, INT r, const void *d, DWORD n)
{ SELF; (void)r; (void)d; (void)n; return S_OK; }
static void __stdcall dev_SetGammaRamp(IDirect3DDevice8 *self, DWORD f, const D3DGAMMARAMP *r) { SELF; (void)f; (void)r; }
static void __stdcall dev_GetGammaRamp(IDirect3DDevice8 *self, D3DGAMMARAMP *r) { SELF; (void)r; }
static HRESULT __stdcall dev_SetPalette(IDirect3DDevice8 *self, DWORD n, const void *e) { SELF; (void)n; (void)e; return S_OK; }
static HRESULT __stdcall dev_BeginPush(IDirect3DDevice8 *self, DWORD n, DWORD **pp) { SELF; (void)n; (void)pp; return E_NOTIMPL; }
static HRESULT __stdcall dev_EndPush(IDirect3DDevice8 *self, DWORD *p) { SELF; (void)p; return E_NOTIMPL; }
static HRESULT __stdcall dev_Swap(IDirect3DDevice8 *self, DWORD f) { SELF; (void)f; host_present(); return S_OK; }

static const IDirect3DDevice8Vtbl g_device_vtbl = {
    dev_QueryInterface, dev_AddRef, dev_Release, dev_GetDirect3D, dev_GetDeviceCaps,
    dev_GetDisplayMode, dev_GetCreationParameters, dev_Reset, dev_Present, dev_GetBackBuffer,
    dev_BeginScene, dev_EndScene, dev_Clear, dev_SetTransform, dev_GetTransform,
    dev_SetRenderState, dev_GetRenderState, dev_SetTextureStageState, dev_GetTextureStageState,
    dev_SetTexture, dev_GetTexture, dev_SetStreamSource, dev_GetStreamSource, dev_SetIndices,
    dev_GetIndices, dev_DrawPrimitive, dev_DrawIndexedPrimitive, dev_DrawPrimitiveUP,
    dev_DrawIndexedPrimitiveUP, dev_CreateTexture, dev_CreateVertexBuffer, dev_CreateIndexBuffer,
    dev_CreateRenderTarget, dev_CreateDepthStencilSurface, dev_SetRenderTarget, dev_GetRenderTarget,
    dev_GetDepthStencilSurface, dev_SetViewport, dev_GetViewport, dev_SetMaterial, dev_GetMaterial,
    dev_SetLight, dev_GetLight, dev_LightEnable, dev_SetVertexShader, dev_GetVertexShader,
    dev_SetVertexShaderConstant, dev_SetPixelShader, dev_GetPixelShader, dev_SetPixelShaderConstant,
    dev_SetGammaRamp, dev_GetGammaRamp, dev_SetPalette, dev_BeginPush, dev_EndPush, dev_Swap,
};

IDirect3DDevice8 *xbox_GetD3DDevice(void) { return g_device_ready ? &g_device : NULL; }
IDirect3DDevice8 *d3d8_GetDevice(void) { return &g_device; }

static void default_states(void)
{
    int st, i;
    memset(g_gl.rs, 0, sizeof g_gl.rs);
    g_gl.rs[D3DRS_ZENABLE] = 1;
    g_gl.rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
    g_gl.rs[D3DRS_SHADEMODE] = 2;
    g_gl.rs[D3DRS_ZWRITEENABLE] = TRUE;
    g_gl.rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    g_gl.rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    g_gl.rs[D3DRS_CULLMODE] = D3DCULL_CCW;
    g_gl.rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    g_gl.rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    g_gl.rs[D3DRS_COLORWRITEENABLE] = 0x0F;
    memset(g_gl.tss, 0, sizeof g_gl.tss);
    for (st = 0; st < GLES_MAX_STAGES; st++) {
        g_gl.tss[st][D3DTSS_COLOROP]   = st == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        g_gl.tss[st][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        g_gl.tss[st][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        g_gl.tss[st][D3DTSS_ALPHAOP]   = st == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        g_gl.tss[st][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        g_gl.tss[st][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        g_gl.tss[st][D3DTSS_TEXCOORDINDEX] = (DWORD)st;
        g_gl.tss[st][D3DTSS_ADDRESSU] = 1;
        g_gl.tss[st][D3DTSS_ADDRESSV] = 1;
        g_gl.tss[st][D3DTSS_MAGFILTER] = 1;
        g_gl.tss[st][D3DTSS_MINFILTER] = 1;
        g_gl.tss[st][D3DTSS_MIPFILTER] = 0;
    }
    g_gl.viewport.X = 0; g_gl.viewport.Y = 0;
    g_gl.viewport.Width = g_gl.guest_w; g_gl.viewport.Height = g_gl.guest_h;
    g_gl.viewport.MinZ = 0.0f; g_gl.viewport.MaxZ = 1.0f;
    for (i = 0; i < GLES_MAX_TRANSFORM; i++) {
        memset(&g_gl.transforms[i], 0, sizeof(D3DMATRIX));
        g_gl.transforms[i].m[0][0] = g_gl.transforms[i].m[1][1] =
        g_gl.transforms[i].m[2][2] = g_gl.transforms[i].m[3][3] = 1.0f;
    }
}

static HRESULT __stdcall d3d_QueryInterface(IDirect3D8 *self, const IID *r, void **p) { SELF; (void)r; (void)p; return E_NOINTERFACE; }
static ULONG __stdcall d3d_AddRef(IDirect3D8 *self) { SELF; return 1; }
static ULONG __stdcall d3d_Release(IDirect3D8 *self) { SELF; return 1; }

static HRESULT __stdcall d3d_CreateDevice(IDirect3D8 *self, UINT adapter, DWORD type, HWND focus,
                                          DWORD flags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice8 **out)
{
    SDL_GLContext ctx;
    SELF; (void)adapter; (void)type; (void)focus; (void)flags;
    if (!pp || !out) return E_INVALIDARG;
    if (g_device_ready) { *out = &g_device; return S_OK; }
    if (!s_window) {
        fprintf(stderr, "[GLES] no host window: d3d8_SetHostWindow was never called\n");
        return E_FAIL;
    }
    ctx = SDL_GL_CreateContext(s_window);
    if (!ctx) {
        fprintf(stderr, "[GLES] OpenGL ES 3.0 context failed: %s\n", SDL_GetError());
        return E_FAIL;
    }
    SDL_GL_MakeCurrent(s_window, ctx);
    {
        const char *e = getenv("XBOX_VSYNC");
        SDL_GL_SetSwapInterval(e && e[0] == '1' ? 1 : 0);
    }
    memset(&g_gl, 0, sizeof g_gl);
    g_gl.window = s_window;
    g_gl.context = ctx;
    g_gl.render_thread = (unsigned long)pthread_self();
    g_gl.guest_w = pp->BackBufferWidth ? pp->BackBufferWidth : 640;
    g_gl.guest_h = pp->BackBufferHeight ? pp->BackBufferHeight : 480;
    g_gl.width = s_cfg_w ? (UINT)s_cfg_w : g_gl.guest_w;
    g_gl.height = s_cfg_h ? (UINT)s_cfg_h : g_gl.guest_h;
    {   /* stay within what the GPU can render to */
        GLint maxrb = 4096;
        glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxrb);
        while ((GLint)g_gl.width > maxrb || (GLint)g_gl.height > maxrb) { g_gl.width /= 2; g_gl.height /= 2; }
    }
    default_states();
    if (!gles_draw_init()) return E_FAIL;
    g_gl.msaa_requested = (UINT)s_want_msaa;
    if (!scene_create(g_gl.msaa_requested)) {
        scene_release();
        if (!scene_create(1)) { fprintf(stderr, "[GLES] scene target failed\n"); return E_FAIL; }
    }
    g_device.lpVtbl = &g_device_vtbl;
    g_device_ready = 1;
    *out = &g_device;
    fprintf(stderr, "[GLES] device: rendering %ux%u (title frame %ux%u), %ux AA, display %.3f\n",
            g_gl.width, g_gl.height, g_gl.guest_w, g_gl.guest_h, g_gl.msaa, d3d8_HostAspect());
    return S_OK;
}

static const IDirect3D8Vtbl g_d3d_vtbl = { d3d_QueryInterface, d3d_AddRef, d3d_Release, d3d_CreateDevice };
static IDirect3D8 g_d3d = { &g_d3d_vtbl };

IDirect3D8 *xbox_Direct3DCreate8(UINT sdk) { (void)sdk; return &g_d3d; }
