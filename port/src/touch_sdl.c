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
 * d3d8_SetPresentOverlay (OpenGL ES 3). The controls fade out while a real
 * controller is in use and come back at the next touch.
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

typedef enum { SH_CIRCLE, SH_RECT } Shape;
typedef struct {
    const char *label;
    Shape shape;
    float x, y, w, h;           /* centre and size; x in screen heights from the edge (see place) */
    int   right;                /* anchored to the right edge */
    WORD  bit;                  /* XINPUT_GAMEPAD_* */
    int   trigger;              /* 1 left, 2 right */
    float r, g, b;
} Button;

/* Positions in units of the screen height, so the controls keep their size
 * and shape on any aspect ratio. */
static const Button k_buttons[] = {
    { "A",     SH_CIRCLE, 0.17f, 0.82f, 0.15f, 0.15f, 1, XINPUT_GAMEPAD_A, 0, 0.30f, 0.80f, 0.25f },
    { "B",     SH_CIRCLE, 0.06f, 0.68f, 0.15f, 0.15f, 1, XINPUT_GAMEPAD_B, 0, 0.90f, 0.25f, 0.20f },
    { "X",     SH_CIRCLE, 0.28f, 0.68f, 0.15f, 0.15f, 1, XINPUT_GAMEPAD_X, 0, 0.25f, 0.45f, 0.95f },
    { "Y",     SH_CIRCLE, 0.17f, 0.54f, 0.15f, 0.15f, 1, XINPUT_GAMEPAD_Y, 0, 0.95f, 0.80f, 0.20f },
    { "LT",    SH_RECT,   0.14f, 0.09f, 0.22f, 0.12f, 0, 0, 1, 0.80f, 0.80f, 0.80f },
    { "RT",    SH_RECT,   0.14f, 0.09f, 0.22f, 0.12f, 1, 0, 2, 0.80f, 0.80f, 0.80f },
    { "LB",    SH_RECT,   0.14f, 0.24f, 0.18f, 0.10f, 0, XINPUT_GAMEPAD_LEFT_SHOULDER, 0, 0.95f, 0.95f, 0.95f },
    { "RB",    SH_RECT,   0.14f, 0.24f, 0.18f, 0.10f, 1, XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0.30f, 0.30f, 0.30f },
    { "BACK",  SH_RECT,   0.50f, 0.07f, 0.20f, 0.08f, 0, XINPUT_GAMEPAD_BACK, 0, 0.60f, 0.60f, 0.60f },
    { "START", SH_RECT,   0.50f, 0.07f, 0.20f, 0.08f, 1, XINPUT_GAMEPAD_START, 0, 0.60f, 0.60f, 0.60f },
};
#define NBUTTONS ((int)(sizeof k_buttons / sizeof k_buttons[0]))

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

/* A button's rectangle in pixels. */
static void place(const Button *b, float *x0, float *y0, float *x1, float *y1)
{
    float H = (float)s_screen_h, cx = b->x * H, cy = b->y * H;
    if (b->label[0] == 'B' && b->label[1] == 'A') cx = s_screen_w * 0.5f - 0.13f * H;      /* BACK  */
    else if (b->label[0] == 'S') cx = s_screen_w * 0.5f + 0.13f * H;                        /* START */
    else if (b->right) cx = s_screen_w - cx;
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
        float R = s_screen_h * 0.12f, dx = (s_stick_x - s_stick_ox) / R, dy = (s_stick_y - s_stick_oy) / R;
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
        s_fingers[slot].used = 0;
    }
    publish();
}

/* ---- drawing (render thread) ----------------------------------------------- */

/* 5x7 glyphs for the labels: A B C K L R S T X Y. */
static const struct { char c; unsigned char row[7]; } k_font[] = {
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
};
#define NGLYPHS ((int)(sizeof k_font / sizeof k_font[0]))

static GLuint s_prog, s_font, s_vao, s_vbo;
static GLint  s_u_rect, s_u_color, s_u_mode, s_u_screen, s_u_uv;

static const char k_vs[] =
    "#version 300 es\n"
    "uniform vec4 u_rect; uniform vec2 u_screen; uniform vec4 u_uv;\n"
    "out vec2 v_p; out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n"
    "    vec2 p = mix(u_rect.xy, u_rect.zw, c);\n"
    "    v_p = c * 2.0 - 1.0; v_uv = mix(u_uv.xy, u_uv.zw, c);\n"
    "    gl_Position = vec4(p.x / u_screen.x * 2.0 - 1.0, 1.0 - p.y / u_screen.y * 2.0, 0.0, 1.0);\n"
    "}\n";
static const char k_fs[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform vec4 u_color; uniform int u_mode; uniform sampler2D u_font;\n"
    "in vec2 v_p; in vec2 v_uv; out vec4 o;\n"
    "void main() {\n"
    "    float a;\n"
    "    if (u_mode == 0) { float d = length(v_p); a = smoothstep(1.0, 0.92, d); }\n"   /* disc */
    "    else if (u_mode == 1) { vec2 q = abs(v_p); a = smoothstep(1.0, 0.94, max(q.x, q.y)); }\n"  /* box */
    "    else if (u_mode == 2) { float d = length(v_p); a = smoothstep(1.0, 0.94, d) * smoothstep(0.86, 0.92, d); }\n"  /* ring */
    "    else a = texture(u_font, v_uv).r;\n"   /* glyph */
    "    o = vec4(u_color.rgb, u_color.a * a);\n"
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
    unsigned char px[NGLYPHS * 6 * 7];
    int g, x, y;
    GLuint v, f;
    if (s_prog) return 1;
    v = sh(GL_VERTEX_SHADER, k_vs); f = sh(GL_FRAGMENT_SHADER, k_fs);
    s_prog = glCreateProgram();
    glAttachShader(s_prog, v); glAttachShader(s_prog, f);
    glLinkProgram(s_prog);
    glDeleteShader(v); glDeleteShader(f);
    s_u_rect = glGetUniformLocation(s_prog, "u_rect");
    s_u_color = glGetUniformLocation(s_prog, "u_color");
    s_u_mode = glGetUniformLocation(s_prog, "u_mode");
    s_u_screen = glGetUniformLocation(s_prog, "u_screen");
    s_u_uv = glGetUniformLocation(s_prog, "u_uv");
    memset(px, 0, sizeof px);
    for (g = 0; g < NGLYPHS; g++)
        for (y = 0; y < 7; y++)
            for (x = 0; x < 5; x++)
                if (k_font[g].row[y] & (0x10 >> x)) px[y * NGLYPHS * 6 + g * 6 + x] = 255;
    glGenTextures(1, &s_font);
    glBindTexture(GL_TEXTURE_2D, s_font);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, NGLYPHS * 6, 7, 0, GL_RED, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenVertexArrays(1, &s_vao);
    (void)s_vbo;
    return 1;
}

static void quad(int mode, float x0, float y0, float x1, float y1, float r, float g, float b, float a)
{
    glUniform4f(s_u_rect, x0, y0, x1, y1);
    glUniform4f(s_u_color, r, g, b, a);
    glUniform1i(s_u_mode, mode);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void text(const char *s, float cx, float cy, float h, float a)
{
    float gw = h * 5.0f / 7.0f, adv = h * 6.0f / 7.0f, x = cx - (adv * (float)strlen(s) - (adv - gw)) * 0.5f;
    for (; *s; s++, x += adv) {
        int g;
        for (g = 0; g < NGLYPHS && k_font[g].c != *s; g++) {}
        if (g == NGLYPHS) continue;
        glUniform4f(s_u_uv, (g * 6) / (float)(NGLYPHS * 6), 0.0f, (g * 6 + 5) / (float)(NGLYPHS * 6), 1.0f);
        quad(3, x, cy - h * 0.5f, x + gw, cy + h * 0.5f, 1, 1, 1, a);
    }
}

/* The controls over the frame. Called by the renderer with the drawable size. */
void touch_draw(int w, int h)
{
    Uint32 now = SDL_GetTicks();
    float fade;
    int i;
    if (!g_touch_active) return;
    /* A real controller in use since the last touch: fade the controls out. */
    fade = (s_last_pad > s_last_touch && now - s_last_pad < 100000) ? 0.0f : 1.0f;
    if (fade <= 0.0f) return;
    if (!draw_init()) return;
    s_screen_w = w; s_screen_h = h;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_font);
    glBindSampler(0, 0);
    glUniform2f(s_u_screen, (float)w, (float)h);
    for (i = 0; i < NBUTTONS; i++) {
        const Button *b = &k_buttons[i];
        float x0, y0, x1, y1, a = (s_pressed & (1 << i)) ? 0.75f : 0.35f;
        place(b, &x0, &y0, &x1, &y1);
        quad(b->shape == SH_CIRCLE ? 0 : 1, x0, y0, x1, y1, b->r, b->g, b->b, a);
        text(b->label, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (y1 - y0) * 0.38f, 0.9f);
    }
    if (s_stick_on) {
        float R = h * 0.12f, k = h * 0.05f, dx = s_stick_x - s_stick_ox, dy = s_stick_y - s_stick_oy;
        float len = sqrtf(dx * dx + dy * dy);
        if (len > R) { dx = dx / len * R; dy = dy / len * R; }
        quad(2, s_stick_ox - R, s_stick_oy - R, s_stick_ox + R, s_stick_oy + R, 1, 1, 1, 0.5f);
        quad(0, s_stick_ox + dx - k, s_stick_oy + dy - k, s_stick_ox + dx + k, s_stick_oy + dy + k, 1, 1, 1, 0.6f);
    }
    glDisable(GL_BLEND);
}

#endif /* !_WIN32 */
