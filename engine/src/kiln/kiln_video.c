/* SPDX-License-Identifier: MIT
 *
 * kiln_video.c — see kiln_video.h.
 */

#include "kiln_video.h"

#include <libdragon.h>
#include <video.h>
#include <yuv.h>
#include <mpeg1.h>

#include <stdlib.h>
#include <string.h>

static int g_video_lib_ready;

static void ensure_video_lib(void)
{
    if (g_video_lib_ready) return;
    yuv_init();
    video_register_codec(&mpeg1_codec);
    g_video_lib_ready = 1;
}

void kiln_video_open(KilnVideo *mv, const char *dfs_path)
{
    memset(mv, 0, sizeof *mv);
    ensure_video_lib();

    video_t *v = video_open(dfs_path, NULL);
    if (!v) {
        debugf("kiln_video: no %s, playing silent/blank\n", dfs_path);
        return;
    }

    const video_info_t info = video_get_info(v);
    mv->v         = v;
    mv->width     = info.width;
    mv->height    = info.height;
    // A stream with no reported rate would divide by zero below; one frame
    // per update() is a safe, if wrong, fallback rather than a crash.
    mv->framerate = info.framerate > 0.0f ? info.framerate : 30.0f;

    // Prime the first frame now, not on the first update()'s throttled
    // decode. video_get_frame() (in draw()) has no "nothing decoded yet"
    // state of its own to fall back on — it assumes at least one
    // video_next_frame() has already succeeded, and calling it before that
    // reads through the decoder's still-unset internal frame pointer. A
    // draw() landing before update() has accumulated a full frame period
    // of dt (routine on the very first frame after open) would hit exactly
    // that with no video_next_frame() ever having run.
    if (!video_next_frame(v)) {
        debugf("kiln_video: %s decoded no frames\n", dfs_path);
        video_close(v);
        return;
    }
    mv->ready = true;
}

bool kiln_video_update(KilnVideo *mv, float dt)
{
    if (!mv->ready) return false;

    mv->frame_accum += dt;
    const float step = 1.0f / mv->framerate;
    // Usually 0 or 1 iterations at ordinary game/video frame rates; the
    // while (rather than if) is what keeps a stalled frame from making the
    // clip drift out of sync with anything it needs to track.
    while (mv->frame_accum >= step) {
        mv->frame_accum -= step;
        if (!video_next_frame((video_t *)mv->v)) return false;
    }
    return true;
}

void kiln_video_draw(KilnVideo *mv, int screen_w, int screen_h)
{
    if (!mv->ready) return;

    if (!mv->blitter_ready) {
        // Deferred to first draw rather than open(): the blitter is sized
        // to the OUTPUT rectangle, which open() has no way to know.
        yuv_blitter_t *b = malloc(sizeof *b);
        *b = yuv_blitter_new_fmv(mv->width, mv->height, screen_w, screen_h,
                                 NULL);
        mv->blitter_block = b;
        mv->blitter_ready = true;
    }

    yuv_frame_t frame = video_get_frame((video_t *)mv->v);
    yuv_blitter_run((yuv_blitter_t *)mv->blitter_block, &frame);
}

void kiln_video_close(KilnVideo *mv)
{
    if (mv->blitter_ready) {
        yuv_blitter_free((yuv_blitter_t *)mv->blitter_block);
        free(mv->blitter_block);
        mv->blitter_block = NULL;
        mv->blitter_ready = false;
    }
    if (mv->ready) {
        video_close((video_t *)mv->v);
        mv->v = NULL;
        mv->ready = false;
    }
}
