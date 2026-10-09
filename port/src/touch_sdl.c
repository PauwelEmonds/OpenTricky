/*
 * touch_sdl.c -- on-screen controls for phones and tablets (Linux too, with
 * OT_TOUCH=1, where the mouse stands in for a finger).
 *
 * The layout is an Xbox pad: a stick that appears where the left thumb
 * lands, A B X Y in the pad's colours on the right, the triggers and the
 * White / Black buttons (LB / RB, as the PC controls map them) in the top
 * corners, BACK and START at the top. Touches become the virtual pad 0 that
 * host_sdl.c merges with a real controller (g_touch_pad).
 *
 * Drawn by the renderer after the game image, on its thread, through
 * d3d8_SetPresentOverlay (OpenGL ES 3), as distance fields: round, soft-
 * shadowed buttons with sharp vector labels at any screen density. The
 * controls hide while a real controller is in use and come back at the
 * next touch.
 */
#ifndef _WIN32

#include <windows.h>
#include <SDL.h>
#include <GLES3/gl3.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

extern XINPUT_GAMEPAD g_touch_pad;
extern volatile int   g_touch_active;

typedef enum { AN_LEFT, AN_RIGHT, AN_CENTRE } Anchor;
typedef struct {
    const char *label;          /* letters, or \x01 View (BACK) / \x02 Menu (START): the glyphs below */
    int   round;                /* a circle; else a pill */
    float x, y, w, h;           /* centre and size in screen heights; x from the anchor's edge (centre: offset) */
    Anchor anchor;
    WORD  bit;                  /* XINPUT_GAMEPAD_* */
    int   trigger;              /* 1 left, 2 right */
    float r, g, b;              /* accent: rim, letter and pressed fill */
} Button;

/* Positions in units of the screen height, so the controls keep their size
 * and shape on any aspect ratio: A B X Y as a diamond on the right, the
 * triggers over the bumpers in the top corners, View and Menu (BACK, START)
 * at the top middle, the options (\x03, no pad button) beside them. */
static const Button k_buttons[] = {
    { "A",    1, 0.21f, 0.80f, 0.135f, 0.135f, AN_RIGHT,  XINPUT_GAMEPAD_A, 0, 0.42f, 0.84f, 0.29f },
    { "B",    1, 0.10f, 0.69f, 0.135f, 0.135f, AN_RIGHT,  XINPUT_GAMEPAD_B, 0, 0.96f, 0.36f, 0.33f },
    { "X",    1, 0.32f, 0.69f, 0.135f, 0.135f, AN_RIGHT,  XINPUT_GAMEPAD_X, 0, 0.29f, 0.58f, 0.98f },
    { "Y",    1, 0.21f, 0.58f, 0.135f, 0.135f, AN_RIGHT,  XINPUT_GAMEPAD_Y, 0, 0.99f, 0.80f, 0.25f },
    { "LT",   0, 0.17f, 0.09f, 0.21f, 0.095f, AN_LEFT,   0, 1, 0.92f, 0.93f, 0.95f },
    { "RT",   0, 0.17f, 0.09f, 0.21f, 0.095f, AN_RIGHT,  0, 2, 0.92f, 0.93f, 0.95f },
    { "LB",   0, 0.17f, 0.21f, 0.18f, 0.085f, AN_LEFT,   XINPUT_GAMEPAD_LEFT_SHOULDER, 0, 0.92f, 0.93f, 0.95f },
    { "RB",   0, 0.17f, 0.21f, 0.18f, 0.085f, AN_RIGHT,  XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0.92f, 0.93f, 0.95f },
    { "\x01", 1, -0.075f, 0.075f, 0.085f, 0.085f, AN_CENTRE, XINPUT_GAMEPAD_BACK, 0, 0.92f, 0.93f, 0.95f },
    { "\x02", 1, 0.075f, 0.075f, 0.085f, 0.085f, AN_CENTRE, XINPUT_GAMEPAD_START, 0, 0.92f, 0.93f, 0.95f },
    { "\x03", 1, 0.25f, 0.075f, 0.075f, 0.075f, AN_CENTRE, 0, 0, 0.92f, 0.93f, 0.95f },
};
#define OPTIONS_BUTTON (NBUTTONS - 1)
#define COUNTER_X (-0.25f)      /* the frame rate counter, from the centre (screen heights) */
#define NBUTTONS ((int)(sizeof k_buttons / sizeof k_buttons[0]))

/* The stick's resting place (shown faintly until a thumb lands), its reach. */
#define STICK_X 0.25f
#define STICK_Y 0.70f
#define STICK_R 0.12f

#define MAX_FINGERS 10
static struct {
    SDL_FingerID id;
    int   used;
    int   button;               /* index into k_buttons, -1 the stick */
} s_fingers[MAX_FINGERS];

static volatile float s_stick_ox, s_stick_oy, s_stick_x, s_stick_y;   /* pixels */
static volatile int   s_stick_on;
static volatile int   s_pressed;                /* bit per button */
static volatile Uint32 s_last_touch, s_last_pad;
static int s_screen_w = 1, s_screen_h = 1;
static volatile int s_options;                  /* the options button was released */

void d3d8_SetShowFps(int on);
int  d3d8_GetShowFps(void);

/* A button's rectangle in pixels. */
static void place(const Button *b, float *x0, float *y0, float *x1, float *y1)
{
    float H = (float)s_screen_h, cx = b->x * H, cy = b->y * H;
    if (b->anchor == AN_CENTRE) cx += s_screen_w * 0.5f;
    else if (b->anchor == AN_RIGHT) cx = s_screen_w - cx;
    *x0 = cx - b->w * H * 0.5f; *x1 = cx + b->w * H * 0.5f;
    *y0 = cy - b->h * H * 0.5f; *y1 = cy + b->h * H * 0.5f;
}

static int hit(float px, float py)
{
    int i;
    for (i = 0; i < NBUTTONS; i++) {
        float x0, y0, x1, y1, pad = s_screen_h * 0.02f;
        place(&k_buttons[i], &x0, &y0, &x1, &y1);
        if (px >= x0 - pad && px <= x1 + pad && py >= y0 - pad && py <= y1 + pad) return i;
    }
    return -1;
}

static void publish(void)
{
    XINPUT_GAMEPAD g;
    int i;
    memset(&g, 0, sizeof g);
    for (i = 0; i < NBUTTONS; i++) {
        if (!(s_pressed & (1 << i))) continue;
        g.wButtons |= k_buttons[i].bit;
        if (k_buttons[i].trigger == 1) g.bLeftTrigger = 255;
        if (k_buttons[i].trigger == 2) g.bRightTrigger = 255;
    }
    if (s_stick_on) {
        float R = s_screen_h * STICK_R, dx = (s_stick_x - s_stick_ox) / R, dy = (s_stick_y - s_stick_oy) / R;
        float len = sqrtf(dx * dx + dy * dy);
        if (len > 1.0f) { dx /= len; dy /= len; }
        g.sThumbLX = (SHORT)(dx * 32767.0f);
        g.sThumbLY = (SHORT)(-dy * 32767.0f);
    }
    g_touch_pad = g;
}

/* Called by the host for every event (main thread). */
void touch_event(const SDL_Event *e, int screen_w, int screen_h)
{
    float px, py;
    int i, slot = -1;
    SDL_FingerID id;
    s_screen_w = screen_w > 0 ? screen_w : 1;
    s_screen_h = screen_h > 0 ? screen_h : 1;
    switch (e->type) {
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERAXISMOTION:
        if (e->type == SDL_CONTROLLERBUTTONDOWN || abs(e->caxis.value) > 12000) s_last_pad = SDL_GetTicks();
        return;
    case SDL_FINGERDOWN: case SDL_FINGERMOTION: case SDL_FINGERUP:
        px = e->tfinger.x * s_screen_w; py = e->tfinger.y * s_screen_h; id = e->tfinger.fingerId;
        break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
        if (e->button.which == SDL_TOUCH_MOUSEID || !getenv("OT_TOUCH")) return;
        px = (float)e->button.x; py = (float)e->button.y; id = 0x7FFF;
        break;
    case SDL_MOUSEMOTION:
        if (e->motion.which == SDL_TOUCH_MOUSEID || !getenv("OT_TOUCH") || !e->motion.state) return;
        px = (float)e->motion.x; py = (float)e->motion.y; id = 0x7FFF;
        break;
    default:
        return;
    }
    for (i = 0; i < MAX_FINGERS; i++) if (s_fingers[i].used && s_fingers[i].id == id) slot = i;

    if (e->type == SDL_FINGERDOWN || e->type == SDL_MOUSEBUTTONDOWN) {
        int b = hit(px, py);
        for (i = 0; slot < 0 && i < MAX_FINGERS; i++) if (!s_fingers[i].used) slot = i;
        if (slot < 0) return;
        s_fingers[slot].used = 1;
        s_fingers[slot].id = id;
        s_last_touch = SDL_GetTicks();
        g_touch_active = 1;
        if (b >= 0) {
            s_fingers[slot].button = b;
            s_pressed |= 1 << b;
        } else if (px < s_screen_w * 0.5f && !s_stick_on) {
            s_fingers[slot].button = -1;                  /* the stick, where the thumb landed */
            s_stick_ox = s_stick_x = px; s_stick_oy = s_stick_y = py;
            s_stick_on = 1;
        } else {
            s_fingers[slot].used = 0;
        }
    } else if (slot >= 0 && (e->type == SDL_FINGERMOTION || e->type == SDL_MOUSEMOTION)) {
        if (s_fingers[slot].button == -1) { s_stick_x = px; s_stick_y = py; }
        else {                                            /* sliding onto another button */
            int b = hit(px, py);
            if (b >= 0 && b != s_fingers[slot].button) {
                s_pressed &= ~(1 << s_fingers[slot].button);
                s_fingers[slot].button = b;
                s_pressed |= 1 << b;
            }
        }
    } else if (slot >= 0) {                               /* up */
        if (s_fingers[slot].button == -1) s_stick_on = 0;
        else s_pressed &= ~(1 << s_fingers[slot].button);
        if (s_fingers[slot].button == OPTIONS_BUTTON) s_options = 1;
        s_fingers[slot].used = 0;
    }
    publish();
}

/* The options button was released: the host opens the options (host_sdl.c).
 * The options take the touches while they are open, so every finger is let
 * go now rather than left held. */
int touch_take_options(void)
{
    if (!s_options) return 0;
    s_options = 0;
    memset(s_fingers, 0, sizeof s_fingers);
    s_pressed = 0;
    s_stick_on = 0;
    publish();
    return 1;
}

/* ---- drawing (render thread) ----------------------------------------------- */

/* The labels as strokes on a 4 x 6 grid (y down), drawn as distance fields:
 * sharp at any size, with round ends. \x01 is the View icon (BACK), \x02
 * the Menu icon (START), as on the Xbox controllers since the One; \x03
 * sliders, the options. Digits and F P S for the frame rate counter. */
#define MAX_SEGS 10
typedef struct { char c; int n; float s[MAX_SEGS][4]; } Glyph;
static const Glyph k_glyphs[] = {
    { 'A', 3, { {0,6,2,0}, {2,0,4,6}, {0.75f,3.9f,3.25f,3.9f} } },
    { 'B', 10, { {0,0,0,6}, {0,0,2.6f,0}, {2.6f,0,3.5f,0.9f}, {3.5f,0.9f,3.5f,2.1f}, {3.5f,2.1f,2.6f,3},
                 {0,3,2.8f,3}, {2.8f,3,3.9f,4}, {3.9f,4,3.9f,5}, {3.9f,5,2.9f,6}, {2.9f,6,0,6} } },
    { 'X', 2, { {0,0,4,6}, {4,0,0,6} } },
    { 'Y', 3, { {0,0,2,3}, {4,0,2,3}, {2,3,2,6} } },
    { 'L', 2, { {0,0,0,6}, {0,6,3.6f,6} } },
    { 'R', 7, { {0,6,0,0}, {0,0,2.7f,0}, {2.7f,0,3.7f,1}, {3.7f,1,3.7f,2}, {3.7f,2,2.7f,3}, {2.7f,3,0,3},
                {2.1f,3,3.9f,6} } },
    { 'T', 2, { {0,0,4,0}, {2,0,2,6} } },
    { '\x01', 8, { {0,2,2.6f,2}, {2.6f,2,2.6f,6}, {2.6f,6,0,6}, {0,6,0,2},
                   {1.4f,0,4,0}, {4,0,4,4}, {4,4,2.6f,4}, {1.4f,0,1.4f,2} } },
    { '\x02', 3, { {0,1,4,1}, {0,3,4,3}, {0,5,4,5} } },
    { '\x03', 6, { {0,1,4,1}, {0,3,4,3}, {0,5,4,5}, {1.2f,0.2f,1.2f,1.8f}, {2.9f,2.2f,2.9f,3.8f}, {1.8f,4.2f,1.8f,5.8f} } },
    { '0', 4, { {0,0,4,0}, {4,0,4,6}, {4,6,0,6}, {0,6,0,0} } },
    { '1', 2, { {0.8f,1.2f,2.2f,0}, {2.2f,0,2.2f,6} } },
    { '2', 5, { {0,0,4,0}, {4,0,4,3}, {4,3,0,3}, {0,3,0,6}, {0,6,4,6} } },
    { '3', 4, { {0,0,4,0}, {4,0,4,6}, {4,6,0,6}, {0.8f,3,4,3} } },
    { '4', 3, { {0,0,0,3.6f}, {0,3.6f,4,3.6f}, {3,0,3,6} } },
    { '5', 5, { {4,0,0,0}, {0,0,0,3}, {0,3,4,3}, {4,3,4,6}, {4,6,0,6} } },
    { '6', 5, { {4,0,0,0}, {0,0,0,6}, {0,6,4,6}, {4,6,4,3}, {4,3,0,3} } },
    { '7', 2, { {0,0,4,0}, {4,0,1.6f,6} } },
    { '8', 5, { {0,0,4,0}, {4,0,4,6}, {4,6,0,6}, {0,6,0,0}, {0,3,4,3} } },
    { '9', 5, { {4,3,0,3}, {0,3,0,0}, {0,0,4,0}, {4,0,4,6}, {4,6,0,6} } },
    { 'F', 3, { {0,0,0,6}, {0,0,4,0}, {0,3,3,3} } },
    { 'P', 6, { {0,6,0,0}, {0,0,2.8f,0}, {2.8f,0,3.8f,1}, {3.8f,1,3.8f,2}, {3.8f,2,2.8f,3}, {2.8f,3,0,3} } },
    { 'S', 10, { {3.6f,0.4f,3,0}, {3,0,1,0}, {1,0,0,1}, {0,1,0,2}, {0,2,1,3}, {1,3,3,3}, {3,3,4,4}, {4,4,4,5},
                 {4,5,3,6}, {3,6,0.4f,6} } },
};
#define NGLYPHS ((int)(sizeof k_glyphs / sizeof k_glyphs[0]))

static GLuint s_prog, s_vao;
static GLint  s_u_rect, s_u_screen, s_u_mode, s_u_fill, s_u_line, s_u_lw, s_u_rad, s_u_soft, s_u_seg, s_u_nseg, s_u_sw;

static const char k_vs[] =
    "#version 300 es\n"
    "uniform vec4 u_rect; uniform vec2 u_screen;\n"
    "out vec2 v_px;\n"                                   /* pixels from the rectangle's centre */
    "void main() {\n"
    "    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n"
    "    vec2 p = mix(u_rect.xy, u_rect.zw, c);\n"
    "    v_px = p - (u_rect.xy + u_rect.zw) * 0.5;\n"
    "    gl_Position = vec4(p.x / u_screen.x * 2.0 - 1.0, 1.0 - p.y / u_screen.y * 2.0, 0.0, 1.0);\n"
    "}\n";
/* mode 0: a rounded box (a circle when the radius is half its size), filled
 * with a little light from above, with a rim; u_soft blurs the edge (shadows).
 * mode 1: a glyph, the segments u_seg in pixels from the centre.
 * Output premultiplied. */
static const char k_fs[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform vec4 u_rect; uniform int u_mode; uniform vec4 u_fill; uniform vec4 u_line;\n"
    "uniform float u_lw; uniform float u_rad; uniform float u_soft;\n"
    "uniform vec4 u_seg[10]; uniform int u_nseg; uniform float u_sw;\n"
    "in vec2 v_px; out vec4 o;\n"
    "float seg(vec2 p, vec2 a, vec2 b) {\n"
    "    vec2 pa = p - a, ba = b - a;\n"
    "    return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0));\n"
    "}\n"
    "void main() {\n"
    "    if (u_mode == 0) {\n"
    "        vec2 half_ = (u_rect.zw - u_rect.xy) * 0.5 - vec2(u_soft);\n"
    "        vec2 q = abs(v_px) - half_ + u_rad;\n"
    "        float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - u_rad;\n"
    "        float aa = max(u_soft, 1.0);\n"
    "        float inside = 1.0 - smoothstep(-0.5 * aa, 0.5 * aa, d);\n"
    "        float rim = u_lw > 0.0 ? clamp(0.5 - (abs(d + u_lw * 0.5) - u_lw * 0.5), 0.0, 1.0) : 0.0;\n"
    "        vec3 f = u_fill.rgb + 0.07 * clamp(-v_px.y / half_.y, -1.0, 1.0);\n"
    "        vec4 base = vec4(f * u_fill.a, u_fill.a) * inside;\n"
    "        vec4 line = vec4(u_line.rgb * u_line.a, u_line.a) * rim;\n"
    "        o = line + base * (1.0 - line.a);\n"
    "    } else {\n"
    "        float d = 1e9;\n"
    "        for (int i = 0; i < u_nseg; i++) d = min(d, seg(v_px, u_seg[i].xy, u_seg[i].zw));\n"
    "        float a = clamp(0.5 - (d - u_sw * 0.5), 0.0, 1.0);\n"
    "        o = vec4(u_fill.rgb * u_fill.a, u_fill.a) * a;\n"
    "    }\n"
    "}\n";

static GLuint sh(GLenum t, const char *src)
{
    GLuint s = glCreateShader(t);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "[TOUCH] %s\n", log); }
    return s;
}

static int draw_init(void)
{
    GLuint v, f;
    if (s_prog) return 1;
    v = sh(GL_VERTEX_SHADER, k_vs); f = sh(GL_FRAGMENT_SHADER, k_fs);
    s_prog = glCreateProgram();
    glAttachShader(s_prog, v); glAttachShader(s_prog, f);
    glLinkProgram(s_prog);
    glDeleteShader(v); glDeleteShader(f);
    s_u_rect = glGetUniformLocation(s_prog, "u_rect");
    s_u_screen = glGetUniformLocation(s_prog, "u_screen");
    s_u_mode = glGetUniformLocation(s_prog, "u_mode");
    s_u_fill = glGetUniformLocation(s_prog, "u_fill");
    s_u_line = glGetUniformLocation(s_prog, "u_line");
    s_u_lw = glGetUniformLocation(s_prog, "u_lw");
    s_u_rad = glGetUniformLocation(s_prog, "u_rad");
    s_u_soft = glGetUniformLocation(s_prog, "u_soft");
    s_u_seg = glGetUniformLocation(s_prog, "u_seg");
    s_u_nseg = glGetUniformLocation(s_prog, "u_nseg");
    s_u_sw = glGetUniformLocation(s_prog, "u_sw");
    glGenVertexArrays(1, &s_vao);
    return 1;
}

/* A rounded box around (cx, cy), half-size hw x hh, corner radius rad; soft
 * blurs it by that many pixels (drawn that much larger). */
static void shape(float cx, float cy, float hw, float hh, float rad, float soft,
                  float fr, float fg, float fb, float fa, float lw, float lr, float lg, float lb, float la)
{
    glUniform1i(s_u_mode, 0);
    glUniform4f(s_u_rect, cx - hw - soft, cy - hh - soft, cx + hw + soft, cy + hh + soft);
    glUniform4f(s_u_fill, fr, fg, fb, fa);
    glUniform4f(s_u_line, lr, lg, lb, la);
    glUniform1f(s_u_lw, lw);
    glUniform1f(s_u_rad, rad);
    glUniform1f(s_u_soft, soft);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* A label centred on (cx, cy), h pixels high. */
static void text(const char *s, float cx, float cy, float h, float r, float g, float b, float a)
{
    float u = h / 6.0f, sw = h * 0.15f, adv = 5.6f * u;
    float x = cx - (adv * (float)(strlen(s) - 1) + 4.0f * u) * 0.5f;
    glUniform1i(s_u_mode, 1);
    glUniform4f(s_u_fill, r, g, b, a);
    glUniform1f(s_u_sw, sw);
    for (; *s; s++, x += adv) {
        float seg[MAX_SEGS][4], gx = x + 2.0f * u;
        int gi, i;
        for (gi = 0; gi < NGLYPHS && k_glyphs[gi].c != *s; gi++) {}
        if (gi == NGLYPHS) continue;
        for (i = 0; i < k_glyphs[gi].n; i++) {          /* grid -> pixels from the glyph's centre */
            seg[i][0] = (k_glyphs[gi].s[i][0] - 2.0f) * u; seg[i][1] = (k_glyphs[gi].s[i][1] - 3.0f) * u;
            seg[i][2] = (k_glyphs[gi].s[i][2] - 2.0f) * u; seg[i][3] = (k_glyphs[gi].s[i][3] - 3.0f) * u;
        }
        glUniform4fv(s_u_seg, k_glyphs[gi].n, &seg[0][0]);
        glUniform1i(s_u_nseg, k_glyphs[gi].n);
        glUniform4f(s_u_rect, gx - 2.0f * u - sw, cy - 3.0f * u - sw, gx + 2.0f * u + sw, cy + 3.0f * u + sw);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
}

/* The controls over the frame. Called by the renderer with the drawable size. */
void touch_draw(int w, int h)
{
    static Uint32 t0;
    static int frames, fps;
    Uint32 now = SDL_GetTicks();
    float H = (float)h, lw = H * 0.0035f, sh_soft = H * 0.02f, sh_dy = H * 0.006f;
    int i, controls, counter = d3d8_GetShowFps();
    /* The frame rate: shown frames, over a second. */
    frames++;
    if (!t0) t0 = now;
    if (now - t0 >= 1000u) { fps = (int)(frames * 1000u / (now - t0)); frames = 0; t0 = now; }
    /* A real controller in use since the last touch: no controls. */
    controls = g_touch_active && !(s_last_pad > s_last_touch && now - s_last_pad < 100000);
    if (!controls && !counter) return;
    if (!draw_init()) return;
    s_screen_w = w; s_screen_h = h;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);         /* premultiplied */
    glBlendEquation(GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glUniform2f(s_u_screen, (float)w, (float)h);

    if (counter) {
        char t[16];
        float cx = w * 0.5f + COUNTER_X * H, cy = 0.075f * H;
        snprintf(t, sizeof t, "%d FPS", fps);
        shape(cx, cy, 0.075f * H, 0.03f * H, 0.03f * H, 0, 0.07f, 0.08f, 0.10f, 0.55f, 0, 0, 0, 0, 0);
        text(t, cx, cy, 0.026f * H, 0.92f, 0.93f, 0.95f, 0.95f);
    }
    if (!controls) goto done;

    for (i = 0; i < NBUTTONS; i++) {
        const Button *b = &k_buttons[i];
        int down = (s_pressed & (1 << i)) != 0;
        float x0, y0, x1, y1, cx, cy, hw, hh, rad, k = down ? 0.93f : 1.0f;
        place(b, &x0, &y0, &x1, &y1);
        cx = (x0 + x1) * 0.5f; cy = (y0 + y1) * 0.5f;
        hw = (x1 - x0) * 0.5f * k; hh = (y1 - y0) * 0.5f * k;
        rad = b->round ? hw : hh;
        if (!down)                                          /* a soft shadow: the button above the picture */
            shape(cx, cy + sh_dy, hw, hh, rad, sh_soft, 0, 0, 0, 0.30f, 0, 0, 0, 0, 0);
        if (down)
            shape(cx, cy, hw, hh, rad, 0, b->r * 0.85f, b->g * 0.85f, b->b * 0.85f, 0.80f,
                  lw, 1, 1, 1, 0.85f);
        else
            shape(cx, cy, hw, hh, rad, 0, 0.07f, 0.08f, 0.10f, 0.42f,
                  lw, b->r, b->g, b->b, b->round && b->anchor != AN_CENTRE ? 0.90f : 0.45f);
        text(b->label, cx, cy, (b->label[0] < 0x20 ? 0.36f : 0.40f) * hh * 2.0f / (b->label[1] ? 1.25f : 1.0f),
             down ? 1.0f : b->r, down ? 1.0f : b->g, down ? 1.0f : b->b, 0.95f);
    }

    {   /* the stick: where the thumb landed, or faintly at its usual place */
        float R = H * STICK_R, k = H * 0.052f, ox, oy, dx = 0, dy = 0;
        if (s_stick_on) {
            float len;
            ox = s_stick_ox; oy = s_stick_oy;
            dx = s_stick_x - ox; dy = s_stick_y - oy;
            len = sqrtf(dx * dx + dy * dy);
            if (len > R) { dx = dx / len * R; dy = dy / len * R; }
        } else {
            ox = STICK_X * H; oy = STICK_Y * H;
        }
        shape(ox, oy, R, R, R, 0, 0.07f, 0.08f, 0.10f, s_stick_on ? 0.35f : 0.18f,
              lw, 1, 1, 1, s_stick_on ? 0.45f : 0.22f);
        shape(ox + dx, oy + dy + sh_dy, k, k, k, sh_soft, 0, 0, 0, s_stick_on ? 0.35f : 0.15f, 0, 0, 0, 0, 0);
        shape(ox + dx, oy + dy, k, k, k, 0, 0.90f, 0.91f, 0.94f, s_stick_on ? 0.90f : 0.30f,
              lw, 1, 1, 1, s_stick_on ? 0.95f : 0.35f);
    }
done:
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_BLEND);
}

#endif /* !_WIN32 */
