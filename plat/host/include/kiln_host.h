/* SPDX-License-Identifier: MIT
 *
 * kiln_host.h — the host backend's own control surface.
 *
 * Everything here is host-only and has no console counterpart, so no engine
 * module and no ROM may include it: a ROM that called kiln_host_capture()
 * would stop building for the N64, which is the one property the whole
 * plat/host arrangement exists to protect. The consumers are the checks and
 * the (eventual) `./dev pc` launcher.
 *
 * ── Why a text manifest and not just pixels ────────────────────────────
 * A HUD regression is almost always "the wrong string, or the right string in
 * the wrong place" — and that is exactly what a pixel diff reports worst.
 * Antialiasing, a one-pixel baseline shift or a palette tweak all light up
 * every glyph, so the diff says "text changed" and nothing more. The manifest
 * records the calls instead: string, position, colour, measured width. It is
 * exact, it diffs as text, and when it does differ it says which label moved.
 *
 * The pixels are still captured, because geometry — panels, bars, reticles —
 * is the half a manifest cannot describe.
 */
#ifndef KILN_HOST_H
#define KILN_HOST_H

#include <stdint.h>
#include <stdio.h>

/** Frames presented since rdpq_init. rdpq_detach_show increments it. */
uint32_t kiln_host_frame(void);

/** Write the current framebuffer to `path` as a PNG. Returns 0 on success. */
int kiln_host_capture(const char *path);

/** Write the text manifest for every rdpq_text_print since the last reset. */
int kiln_host_text_manifest(const char *path);

/** Drop the recorded text runs. Called implicitly by rdpq_attach so a
 *  manifest describes one frame, not an accumulation of them. */
void kiln_host_text_reset(void);

/** Non-transparent pixel count and colour histogram, in the shape
 *  tools/n64-shot.sh prints — so a host capture and a console capture are
 *  read the same way. `top` names how many histogram entries to print. */
void kiln_host_stats(FILE *out, int top);

/** Counters the console cannot report. Reset by rdpq_attach. */
typedef struct {
    uint32_t rects;      /* rdpq_fill_rectangle calls                    */
    uint32_t tris;       /* rdpq_triangle calls                          */
    uint32_t text_runs;  /* rdpq_text_print calls                        */
    uint32_t glyphs;     /* glyphs actually rasterised                   */
    uint32_t missing;    /* codepoints with no glyph in the builtin font */
    uint64_t shaded_px;  /* pixels written, i.e. the fill-rate proxy     */
} KilnHostCounters;

const KilnHostCounters *kiln_host_counters(void);

/* ── the launcher seam ─────────────────────────────────────────────────
 * plat/host is a renderer with no window, no clock and no speaker, and that
 * is deliberate: a gate has to produce the same PNG on every machine, so
 * nothing in here may depend on wall time or on a compositor. A launcher
 * supplies the three things a gate must not have, through this struct.
 *
 * With no hooks installed every function below is exactly what it was — one
 * framebuffer, a frame counter, and audio credited per presented frame — so
 * every check keeps its determinism by construction rather than by remembering
 * to switch something off.
 *
 * `present`  is called from rdpq_detach_show with the finished RGBA8888
 *            framebuffer. w*4 bytes per row, alpha always 255.
 * `vsync`    is called from display_get, which is where the console blocks
 *            until a buffer frees. A launcher paces the frame and pumps its
 *            event queue here, so the blocking point is the same one on both
 *            targets rather than a new concept the console does not have.
 * `audio_free`/`audio_submit` replace the frame-credited buffer model with a
 *            real device's occupancy. Returning 0 from audio_free is what
 *            ends kiln_audio_update's drain loop.
 * Any member may be NULL; the default for that member is used. */
typedef struct {
    void  (*present)(void *ctx, const void *rgba8, int w, int h);
    void  (*vsync)(void *ctx);
    int   (*audio_free)(void *ctx);
    void  (*audio_submit)(void *ctx, const short *stereo, int nsamples);
    void   *ctx;
} KilnHostHooks;

/** Install the launcher hooks. NULL restores the headless defaults. The
 *  pointer is copied, not retained. */
void kiln_host_set_hooks(const KilnHostHooks *hooks);

/** The installed hooks. Never NULL — an all-NULL struct when none are set. */
const KilnHostHooks *kiln_host_hooks(void);

/** Feed the pad. A test calls this directly; a launcher calls it once per
 *  frame from its vsync hook, before the engine's joypad_poll latches. */
/* (declared in <libdragon.h> as kiln_host_pad_set) */

#endif /* KILN_HOST_H */
