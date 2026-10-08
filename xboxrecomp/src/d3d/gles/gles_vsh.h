/* NV2A vertex programs as GLSL (gles_vsh.c); drawn by gles_draw.c. */
#ifndef GLES_VSH_H
#define GLES_VSH_H

#include <stdint.h>
#include "../d3d8_nv2a_vsh.h"

/* Flag in the kind array: a B8G8R8A8 element, read as RGBA and swizzled. */
#define GLES_VSH_IN_BGRA 0x20

int gles_vsh_glsl(const vshcpu_insn *p, int n, uint16_t inputs, const uint8_t kind[16],
                  char *buf, int cap);

#endif
