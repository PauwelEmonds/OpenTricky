/* The register combiners read at run time (gles_psh_uber.c): drawn while a
 * combiner state's own program builds. */
#ifndef GLES_PSH_UBER_H
#define GLES_PSH_UBER_H

#include <stdint.h>
#include "../../nv2a/nv2a_psh.h"

#define GLES_PSH_UBER_WORDS 44      /* the PshUber block: 11 uvec4 */

extern const char gles_psh_uber_fs[];

/* The PshUber block for `st`; 0 if this state needs its own program (the
 * XBOX_NV2A_PSH_SHOW diagnostic). */
int gles_psh_uber_pack(const Nv2aPshState *st, uint32_t out[GLES_PSH_UBER_WORDS]);

#endif
