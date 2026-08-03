/* SPDX-License-Identifier: MPL-2.0
 *
 * Stub libdragon.h for the m64-asset host check. Defines just enough of the
 * libdragon surface m64_asset.c touches so the engine source compiles
 * unmodified on the host. The stub functions record their calls so the
 * check can verify m64_asset_model / m64_asset_sprite routed the right
 * bytes — they do NOT parse anything (libt3d.a is MIPS-only).
 */
#ifndef STUB_LIBDRAGON_H
#define STUB_LIBDRAGON_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* sprite_t shape doesn't matter — m64_asset only reads/writes `flags`. */
typedef struct {
    uint8_t flags;
} sprite_t;

#define SPRITE_FLAGS_TEXFORMAT   0x1F
#define SPRITE_FLAGS_OWNEDBUFFER 0x20
#define SPRITE_FLAGS_NODATA      0x40
#define SPRITE_FLAGS_EXT         0x80

/* Recorded by the stub, inspected by the check main. */
extern void *g_last_sprite_buf;
extern int   g_last_sprite_sz;
extern void *g_last_model_buf;
extern int   g_last_model_sz;

/* Returns `buf` so m64_asset_sprite's "sp == buf" branch is taken and the
 * OWNEDBUFFER flag path is exercised. A real decoder might return a fresh
 * sprite_t; that path is covered by the ROM demo on Ares, not here. */
static inline sprite_t *sprite_load_buf(void *buf, int sz) {
    g_last_sprite_buf = buf;
    g_last_sprite_sz  = sz;
    return (sprite_t *)buf;
}

static inline void sprite_free(sprite_t *s) { (void)s; }

#endif /* STUB_LIBDRAGON_H */