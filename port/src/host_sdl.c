/*
 * host_sdl.c -- the game's host on Linux and Android. See host_sdl.h.
 *
 * Threads:
 *   main      SDL, the window and its events (SDL wants them on one thread);
 *   game      WinMain -> the translated game (big stack: generated code
 *             recurses on the host stack as the Xbox code did on its own);
 *   pump      the push-buffer translator and the GL renderer, started by the
 *             runtime; it makes the GL context current on itself.
 * The input hooks below are called from the game thread and only read state
 * SDL keeps under its own locks.
 */
#ifndef _WIN32

#include "host_sdl.h"
#include "launcher.h"
#include "controls.h"
#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/stat.h>

#ifdef __ANDROID__
#include <jni.h>
#endif

/* Renderer (xboxrecomp/src/d3d/gles). */
void d3d8_SetHostPaused(int paused);
void d3d8_SetMsaa(int n);
int  d3d8_GetMsaa(void);
void d3d8_SetSmaa(int preset);
int  d3d8_GetSmaa(void);
void d3d8_SetAnisotropy(int n);
int  d3d8_GetAnisotropy(void);
void d3d8_SetAo(int on);
int  d3d8_GetAo(void);
void d3d8_SetPresentOverlay(void (*fn)(int w, int h));

/* On-screen controls (touch_sdl.c). */
void touch_event(const SDL_Event *e, int screen_w, int screen_h);
void touch_draw(int w, int h);
int  touch_take_options(void);
int  xinput_hle_press(const char *name, int ms);     /* xapi_input_hle.c */
#include "fps_cap.h"
int  d3d8_monitor_hz(HWND hwnd);
void d3d8_SetShowFps(int on);
int  d3d8_GetShowFps(void);
void d3d8_SetHostPaused(int paused);
void d3d8_SetHostWindow(void *sdl_window);
void d3d8_GlesWindowHints(void);
int  d3d8_HostKey(unsigned vk);
void d3d8_SetScreenshotDir(const char *dir);
void d3d8_SetShaderCacheDir(const char *dir);
void d3d8_HostSetFullscreen(int on);
int  d3d8_HostIsFullscreen(void);

static SDL_Window *s_win;
static char s_data_dir[1024];

/* ---- files -------------------------------------------------------------- */

const char *host_data_dir(void)
{
    if (s_data_dir[0]) return s_data_dir;
    {
        const char *e = getenv("OT_DATA_DIR");
        if (e && *e) { snprintf(s_data_dir, sizeof s_data_dir, "%s", e); return s_data_dir; }
    }
#ifdef __ANDROID__
    {
        const char *p = SDL_AndroidGetExternalStoragePath();
        if (!p) p = SDL_AndroidGetInternalStoragePath();
        snprintf(s_data_dir, sizeof s_data_dir, "%s", p ? p : ".");
    }
#else
    {
        char exe[1024];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            char *sl;
            exe[n] = 0;
            sl = strrchr(exe, '/');
            if (sl) *sl = 0;
            snprintf(s_data_dir, sizeof s_data_dir, "%s", exe);
        } else {
            snprintf(s_data_dir, sizeof s_data_dir, ".");
        }
    }
#endif
    mkdir(s_data_dir, 0755);
    return s_data_dir;
}

void host_display_size(int *w, int *h)
{
    SDL_DisplayMode m;
    if (SDL_WasInit(SDL_INIT_VIDEO) && SDL_GetCurrentDisplayMode(0, &m) == 0 && m.w >= 320 && m.h >= 240) {
        *w = m.w; *h = m.h;
    }
}

void host_open_path(const char *path, BOOL folder)
{
    (void)folder;
    fprintf(stderr, "[HOST] %s\n", path);
}

/* ---- the menu ------------------------------------------------------------ */

#ifdef __ANDROID__
/* OpenTrickyActivity.openDiscImage(forget): the disc image as an open file
 * descriptor -- the one packed into the APK, the one chosen before, or the
 * system's file picker; *offset where the image starts in that file. */
static int java_open_disc(int forget, long long *offset)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    int r = -2;
    if (!env || !act) return -2;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "openDiscImage", "(Z)I");
    if (m) r = (*env)->CallStaticIntMethod(env, c, m, (jboolean)(forget ? JNI_TRUE : JNI_FALSE));
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = -2; }
    *offset = 0;
    m = (*env)->GetStaticMethodID(env, c, "discImageOffset", "()J");
    if (m && r >= 0) *offset = (long long)(*env)->CallStaticLongMethod(env, c, m);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); *offset = 0; }
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
    return r;
}
#endif

static int host_message_box(const char *text, const char *caption, UINT type);

#ifdef __ANDROID__
/* OpenTrickyActivity.options: the graphics options over the paused game.
 * Everything applies at once -- the frame rate (fps_cap_set), the counter,
 * ambient occlusion and SMAA (gles_post.c), multisampling (the scene targets
 * are made again between two frames), texture sharpness -- and is kept in
 * settings.ini (FrameRateLimit, ShowFrameRate, AmbientOcclusion, SmoothEdges,
 * Multisampling, TextureFiltering). The values travel packed in one int, as
 * OpenTrickyActivity.options describes. */
static const int k_msaa_n[4] = { 1, 2, 4, 8 }, k_aniso_n[5] = { 0, 2, 4, 8, 16 };

static int index_of(const int *t, int n, int v)
{
    int i;
    for (i = 0; i < n; i++) if (t[i] == v) return i;
    return 0;
}

static void options_open(void)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    int hz = d3d8_monitor_hz(NULL), fps = fps_cap_current(), r = -1, cur;
    if (!env || !act) return;
    cur = (fps > 61 ? 1 : 0) | (d3d8_GetShowFps() ? 2 : 0)
        | (index_of(k_msaa_n, 4, d3d8_GetMsaa()) << 2) | ((d3d8_GetSmaa() & 7) << 4)
        | (index_of(k_aniso_n, 5, d3d8_GetAnisotropy()) << 7) | (d3d8_GetAo() ? 0x400 : 0);
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "options", "(II)I");
    d3d8_SetHostPaused(1);
    if (m) r = (*env)->CallStaticIntMethod(env, c, m, (jint)cur, (jint)hz);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = -1; }
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
    d3d8_SetHostPaused(0);
    if (r >= 0) {
        LauncherConfig cfg;
        int want = (r & 1) ? hz : 60, counter = (r >> 1) & 1, msaa = k_msaa_n[(r >> 2) & 3];
        int smaa = (r >> 4) & 7, aniso = (r >> 7) & 7, ao = (r >> 10) & 1;
        if (smaa > 4) smaa = 0;
        aniso = k_aniso_n[aniso <= 4 ? aniso : 0];
        d3d8_SetShowFps(counter);
        d3d8_SetMsaa(msaa);
        d3d8_SetSmaa(smaa);
        d3d8_SetAnisotropy(aniso);
        d3d8_SetAo(ao);
        if (!fps_cap_set(want) && want != fps)
            host_message_box("The new frame rate applies from the next start.", "SSX Tricky", 0x40);
        launcher_config_load(&cfg);
        cfg.fps_cap = want > 61 ? -2 : 60;          /* -2: the screen's rate (FrameRateLimit=monitor) */
        cfg.show_fps = counter;
        cfg.msaa = msaa;
        cfg.smaa = smaa;
        cfg.aniso = aniso;
        cfg.ao = ao;
        if (!launcher_config_save(&cfg)) fprintf(stderr, "Could not save the options\n");
        printf("Options:    frame rate %d, counter %s, AO %s, SMAA %d, MSAA %dx, textures %dx\n", want,
               counter ? "on" : "off", ao ? "on" : "off", smaa, msaa, aniso);
    }
}
#endif

BOOL host_launcher_run(struct LauncherConfig *cfg)
{
    /* Called when settings.ini has no disc image that works (main.c). */
#ifdef __ANDROID__
    /* The system's file picker; after an "fd:" image that did not work, it
     * says so. The choice is not written to settings.ini: the app keeps the
     * permission and opens it again at the next start. */
    long long off = 0;
    int fd = java_open_disc(!strncmp(cfg->iso, "fd:", 3), &off);
    if (fd < 0) return FALSE;                     /* cancelled: the app closes */
    if (off > 0) snprintf(cfg->iso, sizeof cfg->iso, "fd:%d@%lld", fd, off);   /* packed in the APK */
    else snprintf(cfg->iso, sizeof cfg->iso, "fd:%d", fd);
    return TRUE;
#else
    char ini[1024];
    launcher_config_path(ini, sizeof ini);
    if (!launcher_config_save(cfg))               /* so there is a file to edit */
        fprintf(stderr, "Could not write %s\n", ini);
    fprintf(stderr, "No usable disc image. Put DiscImage=/path/to/SSX Tricky (USA).iso in [Game]\n"
                    "of %s, or start with the image on the command line.\n", ini);
    host_message_box("No usable disc image is set.\n\nSet DiscImage in [Game] of settings.ini "
                     "beside the game, or start it with the disc image on the command line.",
                     "SSX Tricky", 0x30);
    return FALSE;
#endif
}

/* ---- message boxes ------------------------------------------------------- */

static int host_message_box(const char *text, const char *caption, UINT type)
{
    Uint32 fl = (type & 0x10) ? SDL_MESSAGEBOX_ERROR :
                (type & 0x30) == 0x30 ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION;
    SDL_ShowSimpleMessageBox(fl, caption ? caption : "SSX Tricky", text ? text : "", s_win);
    return IDOK;
}

/* ---- keyboard ------------------------------------------------------------- */

static SDL_Scancode vk_to_scancode(int vk)
{
    if (vk >= 'A' && vk <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (vk - '1'));
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= VK_F1 && vk <= VK_F12) return (SDL_Scancode)(SDL_SCANCODE_F1 + (vk - VK_F1));
    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) return (SDL_Scancode)(SDL_SCANCODE_KP_1 + (vk - VK_NUMPAD1));
    switch (vk) {
    case VK_NUMPAD0: return SDL_SCANCODE_KP_0;
    case VK_RETURN: return SDL_SCANCODE_RETURN;
    case VK_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case VK_SPACE: return SDL_SCANCODE_SPACE;
    case VK_BACK: return SDL_SCANCODE_BACKSPACE;
    case VK_TAB: return SDL_SCANCODE_TAB;
    case VK_LEFT: return SDL_SCANCODE_LEFT;
    case VK_RIGHT: return SDL_SCANCODE_RIGHT;
    case VK_UP: return SDL_SCANCODE_UP;
    case VK_DOWN: return SDL_SCANCODE_DOWN;
    case VK_PRIOR: return SDL_SCANCODE_PAGEUP;
    case VK_NEXT: return SDL_SCANCODE_PAGEDOWN;
    case VK_HOME: return SDL_SCANCODE_HOME;
    case VK_END: return SDL_SCANCODE_END;
    case VK_INSERT: return SDL_SCANCODE_INSERT;
    case VK_DELETE: return SDL_SCANCODE_DELETE;
    case VK_LSHIFT: return SDL_SCANCODE_LSHIFT;
    case VK_RSHIFT: return SDL_SCANCODE_RSHIFT;
    case VK_LCONTROL: return SDL_SCANCODE_LCTRL;
    case VK_RCONTROL: return SDL_SCANCODE_RCTRL;
    case VK_LMENU: return SDL_SCANCODE_LALT;
    case VK_RMENU: return SDL_SCANCODE_RALT;
    case VK_ADD: return SDL_SCANCODE_KP_PLUS;
    case VK_SUBTRACT: return SDL_SCANCODE_KP_MINUS;
    case VK_MULTIPLY: return SDL_SCANCODE_KP_MULTIPLY;
    case VK_DIVIDE: return SDL_SCANCODE_KP_DIVIDE;
    case VK_DECIMAL: return SDL_SCANCODE_KP_PERIOD;
    case VK_OEM_1: return SDL_SCANCODE_SEMICOLON;
    case VK_OEM_PLUS: return SDL_SCANCODE_EQUALS;
    case VK_OEM_COMMA: return SDL_SCANCODE_COMMA;
    case VK_OEM_MINUS: return SDL_SCANCODE_MINUS;
    case VK_OEM_PERIOD: return SDL_SCANCODE_PERIOD;
    case VK_OEM_2: return SDL_SCANCODE_SLASH;
    case VK_OEM_3: return SDL_SCANCODE_GRAVE;
    case VK_OEM_4: return SDL_SCANCODE_LEFTBRACKET;
    case VK_OEM_5: return SDL_SCANCODE_BACKSLASH;
    case VK_OEM_6: return SDL_SCANCODE_RIGHTBRACKET;
    case VK_OEM_7: return SDL_SCANCODE_APOSTROPHE;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

static SHORT host_async_key_state(int vk)
{
    int n = 0;
    const Uint8 *k = SDL_GetKeyboardState(&n);
    SDL_Scancode sc;
    if (!k) return 0;
    if (vk == VK_SHIFT)   return (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) ? (SHORT)0x8000 : 0;
    if (vk == VK_CONTROL) return (k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL]) ? (SHORT)0x8000 : 0;
    if (vk == VK_MENU)    return (k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT]) ? (SHORT)0x8000 : 0;
    sc = vk_to_scancode(vk);
    if (sc == SDL_SCANCODE_UNKNOWN || (int)sc >= n) return 0;
    /* Only while the game window has the keyboard, as on Windows. */
    if (s_win && !(SDL_GetWindowFlags(s_win) & SDL_WINDOW_INPUT_FOCUS)) return 0;
    return k[sc] ? (SHORT)0x8000 : 0;
}

/* The game window when it has the keyboard (controls.c reads keys only then). */
static HWND host_foreground_window(void)
{
    if (s_win && (SDL_GetWindowFlags(s_win) & SDL_WINDOW_INPUT_FOCUS)) return (HWND)s_win;
    return NULL;
}

/* ---- controllers ----------------------------------------------------------- */

#define HOST_PADS 4
static SDL_GameController *s_pads[HOST_PADS];
static pthread_mutex_t s_pad_lock = PTHREAD_MUTEX_INITIALIZER;

/* Touch pad (Android): the on-screen controls write here; read as pad 0
 * merged with a real controller if there is one. */
XINPUT_GAMEPAD g_touch_pad;
volatile int   g_touch_active;

static void pads_open(int device_index)
{
    int i;
    if (!SDL_IsGameController(device_index)) return;
    pthread_mutex_lock(&s_pad_lock);
    for (i = 0; i < HOST_PADS; i++)
        if (!s_pads[i]) {
            s_pads[i] = SDL_GameControllerOpen(device_index);
            if (s_pads[i])
                fprintf(stderr, "[HOST] controller %d: %s\n", i, SDL_GameControllerName(s_pads[i]));
            break;
        }
    pthread_mutex_unlock(&s_pad_lock);
}

static void pads_close(SDL_JoystickID id)
{
    int i;
    pthread_mutex_lock(&s_pad_lock);
    for (i = 0; i < HOST_PADS; i++)
        if (s_pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pads[i])) == id) {
            SDL_GameControllerClose(s_pads[i]);
            s_pads[i] = NULL;
        }
    pthread_mutex_unlock(&s_pad_lock);
}

static DWORD host_xinput_get_state(DWORD idx, XINPUT_STATE *st)
{
    static DWORD packet;
    SDL_GameController *c;
    WORD b = 0;
    if (!st) return ERROR_INVALID_PARAMETER;
    memset(st, 0, sizeof *st);
    if (idx >= HOST_PADS) return ERROR_DEVICE_NOT_CONNECTED;
    pthread_mutex_lock(&s_pad_lock);
    c = s_pads[idx];
    if (c && SDL_GameControllerGetAttached(c)) {
        XINPUT_GAMEPAD *g = &st->Gamepad;
        static const struct { SDL_GameControllerButton s; WORD x; } map[] = {
            { SDL_CONTROLLER_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP },
            { SDL_CONTROLLER_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
            { SDL_CONTROLLER_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT },
            { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
            { SDL_CONTROLLER_BUTTON_START, XINPUT_GAMEPAD_START },
            { SDL_CONTROLLER_BUTTON_BACK, XINPUT_GAMEPAD_BACK },
            { SDL_CONTROLLER_BUTTON_LEFTSTICK, XINPUT_GAMEPAD_LEFT_THUMB },
            { SDL_CONTROLLER_BUTTON_RIGHTSTICK, XINPUT_GAMEPAD_RIGHT_THUMB },
            { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER },
            { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER },
            { SDL_CONTROLLER_BUTTON_A, XINPUT_GAMEPAD_A },
            { SDL_CONTROLLER_BUTTON_B, XINPUT_GAMEPAD_B },
            { SDL_CONTROLLER_BUTTON_X, XINPUT_GAMEPAD_X },
            { SDL_CONTROLLER_BUTTON_Y, XINPUT_GAMEPAD_Y },
        };
        size_t k;
        for (k = 0; k < sizeof map / sizeof map[0]; k++)
            if (SDL_GameControllerGetButton(c, map[k].s)) b |= map[k].x;
        g->wButtons = b;
        g->bLeftTrigger = (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
        g->bRightTrigger = (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
        g->sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        g->sThumbLY = (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
        g->sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
        g->sThumbRY = (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));
        pthread_mutex_unlock(&s_pad_lock);
    } else {
        pthread_mutex_unlock(&s_pad_lock);
        if (!(idx == 0 && g_touch_active)) return ERROR_DEVICE_NOT_CONNECTED;
    }
    if (idx == 0 && g_touch_active) {
        XINPUT_GAMEPAD *g = &st->Gamepad, t = g_touch_pad;
        g->wButtons |= t.wButtons;
        if (t.bLeftTrigger > g->bLeftTrigger) g->bLeftTrigger = t.bLeftTrigger;
        if (t.bRightTrigger > g->bRightTrigger) g->bRightTrigger = t.bRightTrigger;
        if (t.sThumbLX || t.sThumbLY) { g->sThumbLX = t.sThumbLX; g->sThumbLY = t.sThumbLY; }
        if (t.sThumbRX || t.sThumbRY) { g->sThumbRX = t.sThumbRX; g->sThumbRY = t.sThumbRY; }
    }
    st->dwPacketNumber = ++packet;
    return ERROR_SUCCESS;
}

/* ---- window keys ------------------------------------------------------------ */

static unsigned sdl_key_to_vk(SDL_Keycode k)
{
    if (k >= SDLK_F1 && k <= SDLK_F12) return VK_F1 + (unsigned)(k - SDLK_F1);
    if (k >= SDLK_a && k <= SDLK_z) return 'A' + (unsigned)(k - SDLK_a);
    if (k == SDLK_ESCAPE) return VK_ESCAPE;
    if (k == SDLK_RETURN) return VK_RETURN;
    return 0;
}

/* ---- the app in the background (Android) -------------------------------------
 * Android takes the window's surface away while the app is not shown; the
 * renderer waits instead of drawing into nothing, and picks the surface up
 * again on return. Called on the thread that posts the event (SDL's Java
 * side), so it reaches the renderer even while the main loop is blocked. */
/* A race running when the app went away: on return the game's own pause
 * menu is opened (START), so the player picks up where they choose. A race
 * already paused (its tick counter standing still) is left alone: START
 * would resume it. race_watch samples the race on the main loop. */
static volatile int s_race_running, s_pause_on_return, s_returned;
static volatile Uint32 s_returned_at;

static void race_watch(void)
{
    static uint32_t last_tick;
    static Uint32 last_at;
    Uint32 now = SDL_GetTicks();
    uint32_t tick = 0;
    int st;
    if (now - last_at < 250u) return;
    st = game_race_state(&tick);
    s_race_running = st == 4 && tick != last_tick && last_at != 0;
    last_tick = tick;
    last_at = now;
    /* back in front: once the renderer draws again, START */
    if (s_pause_on_return && s_returned && now - s_returned_at > 700u) {
        s_pause_on_return = 0;
        xinput_hle_press("start", 150);
        fprintf(stderr, "Back in front: the race is paused (START)\n");
    }
}

static int SDLCALL lifecycle_watch(void *ud, SDL_Event *e)
{
    (void)ud;
    if (e->type == SDL_APP_WILLENTERBACKGROUND) {
        if (!s_pause_on_return) s_pause_on_return = s_race_running;
        s_returned = 0;
        d3d8_SetHostPaused(1);
    } else if (e->type == SDL_APP_DIDENTERFOREGROUND) {
        s_returned_at = SDL_GetTicks();
        s_returned = 1;
        d3d8_SetHostPaused(0);
    }
    return 1;
}

/* ---- the run ---------------------------------------------------------------- */

static int (*s_game_main)(void);
static volatile int s_game_done;
static int s_game_rc;

static void *game_thread(void *arg)
{
    (void)arg;
    s_game_rc = s_game_main();
    s_game_done = 1;
    {
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_QUIT;
        SDL_PushEvent(&e);
    }
    return NULL;
}

/* Xlib is not thread-safe unless told so, and the renderer swaps buffers on
 * its own thread while this one handles events. */
static void x11_threads(void)
{
#if !defined(__ANDROID__)
    void *x = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
    if (x) {
        int (*init)(void) = (int (*)(void))dlsym(x, "XInitThreads");
        if (init) init();
    }
#endif
}

int host_run(int (*game_main)(void))
{
    LauncherConfig cfg;
    pthread_attr_t attr;
    pthread_t th;
    int w = 1280, h = 960, dw = 0, dh = 0;
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;

    /* settings.ini, the saves and the log live in the data folder
     * (settings.c's game folder is OT_DATA_DIR). */
    setenv("OT_DATA_DIR", host_data_dir(), 0);
#ifdef __ANDROID__
    /* A phone is wider than 16:9 and has no settings screen: the game's own
     * widescreen menus (Menus=16:9) rather than 4:3 ones between wide bars.
     * Races fill the screen either way (ScreenShape=auto). */
    setenv("XBOX_WIDE_MENUS", "16:9", 0);
    setenv("XBOX_FPS_CAP_LIVE", "1", 0);          /* the options switch 60 / 120 while playing */
    setenv("XBOX_PASS_TAGS", "emit", 0);          /* the end of the 3D, where the options' AO / SMAA run */
    {   /* Android drops stdout and stderr: everything goes to the log in the
         * app's folder, replaced at each start, for bug reports. */
        char log[1100];
        snprintf(log, sizeof log, "%s/SSX Tricky.log", host_data_dir());
        if (freopen(log, "w", stdout)) setvbuf(stdout, NULL, _IONBF, 0);
        if (freopen(log, "a", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
    }
#endif
    x11_threads();
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    /* EGL rather than GLX on X11: new shaders are built on a second, shared
     * EGL context (gles_progcache.c). */
    SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    g_w32_message_box = host_message_box;
    g_w32_async_key_state = host_async_key_state;
    g_w32_xinput_get_state = host_xinput_get_state;
    g_w32_foreground_window = host_foreground_window;
    {
        char shots[1100];
        snprintf(shots, sizeof shots, "%s/Screenshots", host_data_dir());
        d3d8_SetScreenshotDir(shots);
        snprintf(shots, sizeof shots, "%s/ShaderCache", host_data_dir());
        d3d8_SetShaderCacheDir(shots);
    }

    /* The window at the configured size, shrunk to fit the display. */
    launcher_config_load(&cfg);
    if (cfg.width >= 320 && cfg.height >= 240) { w = cfg.width; h = cfg.height; }
    host_display_size(&dw, &dh);
    if (dw && dh && (w > dw * 9 / 10 || h > dh * 9 / 10)) {
        double k = (double)(dw * 9 / 10) / w, k2 = (double)(dh * 9 / 10) / h;
        if (k2 < k) k = k2;
        w = (int)(w * k); h = (int)(h * k);
    }
#ifdef __ANDROID__
    flags |= SDL_WINDOW_FULLSCREEN;
#else
    if (cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
    d3d8_GlesWindowHints();
    s_win = SDL_CreateWindow("SSX Tricky", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!s_win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    d3d8_SetHostWindow(s_win);
    SDL_AddEventWatch(lifecycle_watch, NULL);
    d3d8_SetPresentOverlay(touch_draw);

    s_game_main = game_main;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u * 1024u * 1024u);
    if (pthread_create(&th, &attr, game_thread, NULL) != 0) {
        fprintf(stderr, "could not start the game thread\n");
        return 1;
    }
    pthread_attr_destroy(&attr);

    for (;;) {
        SDL_Event e;
        race_watch();
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        {
            int dw = 0, dh = 0;
            SDL_GL_GetDrawableSize(s_win, &dw, &dh);
            touch_event(&e, dw, dh);
        }
#ifdef __ANDROID__
        if (touch_take_options() || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_AC_BACK && !e.key.repeat)) {
            options_open();
            continue;
        }
#endif
        switch (e.type) {
        case SDL_QUIT:
            fflush(stdout);
            fflush(stderr);
            _exit(s_game_done ? s_game_rc : 0);
        case SDL_KEYDOWN:
            if (e.key.repeat) break;
            if (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)) {
                d3d8_HostSetFullscreen(!d3d8_HostIsFullscreen());
                break;
            }
            {
                unsigned vk = sdl_key_to_vk(e.key.keysym.sym);
                if (vk) d3d8_HostKey(vk);
            }
            break;
        case SDL_CONTROLLERDEVICEADDED:
            pads_open(e.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            pads_close((SDL_JoystickID)e.cdevice.which);
            break;
        default:
            break;
        }
    }
}

#endif /* !_WIN32 */
