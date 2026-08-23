/* SPDX-License-Identifier: MIT
 *
 * kiln_video.h — frame-by-frame FMV decode, for shots that need a video clip
 * composited into their own per-frame draw rather than owning the whole
 * screen.
 *
 * libdragon's fmv.h already provides a one-call `fmv_play()`, but it BLOCKS
 * for the whole clip and drives its own internal loop — the wrong shape for
 * this engine, where every screen is a `setup`/`update`/`draw`/`teardown`
 * callback set polled once per frame by the caller's own loop (kiln_skel.h's
 * update/draw split is the model). This is a thin wrapper over the
 * mid-level pieces `fmv_play` itself is built from — video.h's decode API
 * plus yuv.h's FMV-shaped blitter — so a shot can pump one frame of decode
 * per `kiln_video_update` call and blit it inside its own `draw`, the same
 * way it would call `kiln_skel_draw`.
 *
 * ── MPEG1 only, for now ─────────────────────────────────────────────────
 * The MPEG1 codec (`mpeg1_codec`, extension `.m1v`) is registered
 * unconditionally on first use. H.264 exists in libdragon too but nothing
 * here needs it yet — add a second `video_register_codec` call if that
 * changes.
 *
 * ── Missing or unopenable file is silent, not fatal ─────────────────────
 * Same contract as every other kiln_ loader: a video that failed to open
 * leaves `ready` false, `kiln_video_update` returns false immediately (the
 * caller's normal "stream ended, move on" path), and `kiln_video_draw` is a
 * no-op. A shot wanting a placeholder for a not-yet-authored clip needs no
 * special-casing — it just falls straight through to whatever comes next.
 *
 * ── No audio here ───────────────────────────────────────────────────────
 * Deliberately not wired to wav64 — a caller that wants a synced audio
 * track loads and plays it itself (kiln_sfx_load/play), same as any other
 * screen's music.
 */
#ifndef KILN_VIDEO_H
#define KILN_VIDEO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *v;              /* video_t*, opaque here so this header does not
                            * have to pull in <video.h>/<yuv.h> at every
                            * include site. */
    void *blitter_block;  /* yuv_blitter_t's rspq_block_t*, or NULL until
                            * the first draw (it needs the screen size,
                            * which open() does not have). */
    int   width, height;
    float framerate;      /* the stream's own fps, so update() can decode
                            * at the video's real speed regardless of the
                            * caller's frame rate. */
    float frame_accum;    /* seconds banked toward the next decoded frame */
    bool  ready;
    bool  blitter_ready;
} KilnVideo;

/** Open a video for frame-by-frame decode. `dfs_path` is a `rom:/...` path
 *  to a raw `.m1v` elementary stream. On failure (missing file, wrong
 *  extension, decode-header error) `mv->ready` is left false and every
 *  other call on this handle becomes a safe no-op. */
void kiln_video_open(KilnVideo *mv, const char *dfs_path);

/** Advance playback by `dt` seconds, decoding as many frames as the
 *  stream's own framerate calls for (usually 0 or 1 at typical rates —
 *  this is what stops a 15 fps clip playing back 4x too fast just because
 *  the caller polls at 60 Hz). Returns false once the stream ends — or was
 *  never opened — which is the shot's "this beat is over" signal. */
bool kiln_video_update(KilnVideo *mv, float dt);

/** Blit the most recently decoded frame into the currently attached
 *  surface, centred and scaled to fit `screen_w` x `screen_h` with
 *  letterboxing on an aspect-ratio mismatch (yuv_blitter_new_fmv's own
 *  default). Call inside the 2D/GUI pass. No-op if not ready. */
void kiln_video_draw(KilnVideo *mv, int screen_w, int screen_h);

/** Close the video and release its decoder/blitter state. Safe on a
 *  handle that never opened. */
void kiln_video_close(KilnVideo *mv);

#ifdef __cplusplus
}
#endif

#endif /* KILN_VIDEO_H */
