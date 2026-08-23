/* SPDX-License-Identifier: MIT
 *
 * Stub libdragon.h for the host checks (kiln-asset.nix, kiln-logic.nix).
 *
 * Defines just enough of the libdragon surface the engine modules under host
 * test touch so their sources compile UNMODIFIED on the host — no #ifdefs in
 * engine code for the benefit of a test. The stub functions record their calls
 * so a check can verify routing (e.g. kiln_asset_model handing on the right
 * bytes) without parsing anything, since libt3d.a is MIPS-only.
 */
#ifndef STUB_LIBDRAGON_H
#define STUB_LIBDRAGON_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── debugf / assertf ──────────────────────────────────────────────────
 * debugf goes to stderr rather than nowhere: several modules under test use
 * it to report a policy decision the check wants to see (kiln_event's
 * pool-full eviction, kiln_cache's refcount complaints), and a check that
 * discarded them would be unable to distinguish "handled and reported" from
 * "silently did nothing".
 *
 * assertf ABORTS, matching libdragon's semantics rather than softening them.
 * kiln_clip.c uses it for the caller-contract violations it refuses to guess
 * about; a stub that merely logged would let a check pass on input the
 * console would have died on. */
#define debugf(...) fprintf(stderr, __VA_ARGS__)

#define assertf(cond, ...) do {                                             \
        if (!(cond)) {                                                      \
            fprintf(stderr, "ASSERTION FAILED: %s\n  ", #cond);             \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n  at %s:%d\n", __FILE__, __LINE__);          \
            abort();                                                        \
        }                                                                   \
    } while (0)

/* sprite_t shape doesn't matter — kiln_asset only reads/writes `flags`. */
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

/* Returns `buf` so kiln_asset_sprite's "sp == buf" branch is taken and the
 * OWNEDBUFFER flag path is exercised. A real decoder might return a fresh
 * sprite_t; that path is covered by the ROM demo on Ares, not here. */
static inline sprite_t *sprite_load_buf(void *buf, int sz) {
    g_last_sprite_buf = buf;
    g_last_sprite_sz  = sz;
    return (sprite_t *)buf;
}

static inline void sprite_free(sprite_t *s) { (void)s; }

#endif /* STUB_LIBDRAGON_H */