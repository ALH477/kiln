/* SPDX-License-Identifier: MPL-2.0
 *
 * Host shim for libdragon's fast-math header. The engine uses fm_sinf/fm_cosf
 * because they inline to a polynomial instead of calling libm on a VR4300;
 * on the host the accuracy difference is irrelevant to a UI preview, so these
 * are the real thing. Anything that DEPENDED on fm_*'s approximation error
 * would be a bug on hardware too.
 */
#ifndef KILN_UIPREVIEW_FMATH_H
#define KILN_UIPREVIEW_FMATH_H
#include <math.h>
static inline float fm_sinf(float x) { return sinf(x); }
static inline float fm_cosf(float x) { return cosf(x); }
#endif
