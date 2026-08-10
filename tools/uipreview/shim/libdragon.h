/* SPDX-License-Identifier: MPL-2.0
 *
 * Host shim for <libdragon.h>, enough of it to compile m64_widget.c natively.
 *
 * m64_widget.c touches exactly two things from libdragon: the color_t struct
 * and the RGBA32 macro. Everything else it needs it gets through m64_gui.h,
 * whose functions the preview harness implements itself. So this is not a
 * libdragon emulation and must not grow into one — if a widget ever needs a
 * real rdpq call, that is a signal the widget belongs below m64_gui, not
 * that this file needs more in it.
 */
#ifndef M64_UIPREVIEW_LIBDRAGON_H
#define M64_UIPREVIEW_LIBDRAGON_H

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

typedef struct { uint8_t r, g, b, a; } color_t;

#define RGBA32(rr, gg, bb, aa) \
    ((color_t){ .r = (uint8_t)(rr), .g = (uint8_t)(gg), \
                .b = (uint8_t)(bb), .a = (uint8_t)(aa) })

#endif
