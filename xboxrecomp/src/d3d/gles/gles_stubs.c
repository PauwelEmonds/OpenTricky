/*
 * OpenGL ES 3 renderer -- the Windows renderer's extras that are not ported
 * (yet): the post chain and SMAA (d3d8_post.c), soft shadows
 * (d3d8_softshadow.c) and the GPU timing profiler (d3d8_gpuprof.c). Off here,
 * which is also their default on Windows, so the image is the title's own.
 * Plus the translator's pump switches that live in d3d8_nv2a.c there.
 */
#include "gles_internal.h"
#include "../d3d8_gpuprof.h"
#include <stdlib.h>

/* post chain */
int  d3d8_post_split_wanted(void) { return 0; }
void d3d8_PassPhaseChanged(int old_phase, int new_phase, unsigned tag)
{ (void)old_phase; (void)new_phase; (void)tag; }
void d3d8_SetPostProcess(int on) { (void)on; }
int  d3d8_GetPostProcess(void) { return 0; }
void d3d8_SetSmaa(int preset) { (void)preset; }
int  d3d8_GetSmaa(void) { return 0; }
void d3d8_SetPostSplit(int on) { (void)on; }
int  d3d8_GetPostSplit(void) { return 0; }

/* soft shadows: the title's own stencil shadow quad is drawn as on Xbox */
int d3d8_SoftShadowOn(void) { return 0; }
int d3d8_SoftShadowDraw(float shade, unsigned cmp, unsigned ref, unsigned rmask,
                        float fx0, float fy0, float fx1, float fy1)
{ (void)shade; (void)cmp; (void)ref; (void)rmask; (void)fx0; (void)fy0; (void)fx1; (void)fy1; return 0; }

/* GPU profiler */
int  g_gpuprof_on = 0;
void gpuprof_init(void) {}
void gpuprof_set_phase(int phase, int group) { (void)phase; (void)group; }
int  gpuprof_draw_cat(void) { return GP_OTHER; }
void gpuprof_begin(int cat) { (void)cat; }
void gpuprof_end(void) {}
void gpuprof_draw_begin(void) {}
void gpuprof_draw_end(void) {}
void gpuprof_count_flush(void) {}
void gpuprof_note(unsigned bits) { (void)bits; }
void gpuprof_frame(void) {}

/* The translator's pump optimisations (d3d8_nv2a.c on Windows): the same
 * switches, defaults on; XBOX_FIX_PUMP_ALT=1 alternates them per frame. */
static unsigned g_pump_frame;
static int pump_alt_off(void)
{
    static int alt = -1;
    if (alt < 0) { const char *e = getenv("XBOX_FIX_PUMP_ALT"); alt = e && e[0] == '1'; }
    return alt && (g_pump_frame & 1u);
}
void d3d8_pump_frame_tick(void) { g_pump_frame++; }
int  d3d8_pump_alt_off(void) { return pump_alt_off(); }
int  d3d8_pump_state_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_PUMP_STATE"); on = !(e && e[0] == '0'); }
    return on && !pump_alt_off();
}
int  d3d8_pump_cb_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_PUMP_CB"); on = !(e && e[0] == '0'); }
    return on && !pump_alt_off();
}
int  d3d8_pump_cache_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("XBOX_FIX_PUMP_CACHE"); on = !(e && e[0] == '0'); }
    return on && !pump_alt_off();
}
