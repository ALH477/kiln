/* SPDX-License-Identifier: MIT
 *
 * host_video.c — fig_video's four entry points, on the host.
 *
 * The second place plat/host implements a kiln_* function rather than a
 * libdragon or Tiny3D one, and for the same reason host_panic.c is the first:
 * the console version is not engine logic, it is a decoder. kiln_video.c is
 * libdragon's MPEG1 path — <video.h>, <yuv.h>, <mpeg1.h> — plus a YUV blitter
 * recorded into an rspq block. None of that has a host analogue short of
 * vendoring a video codec, and a fake decoder that produced grey frames would
 * be worse than nothing: it would look like the video working.
 *
 * ── Why this is honest rather than a stub ─────────────────────────────
 * fig_video's contract already has a "no video" answer, and the game already
 * takes it. src/pm_credits.c reads:
 *
 *     if (!fig_video_update(&g_video, dt)) g_phase = PHASE_HOLD;
 *
 * PHASE_HOLD is the static credits card, which is what plays when the FMV is
 * absent from a ROM too. So the host build runs the credits exactly as a ROM
 * built without the video asset does — a path the game supports on purpose,
 * not a path invented here.
 *
 * What this costs: the FMV itself cannot be judged natively. That is a real
 * limit and it is the ROM's job, like fill rate.
 */
#include <libdragon.h>
#include <kiln/kiln_video.h>

#include <string.h>

void fig_video_open(FigVideo *mv, const char *dfs_path)
{
    if (!mv) return;
    memset(mv, 0, sizeof *mv);
    /* Once, and saying which file, because the alternative is a credits roll
     * that silently shows a card and a reader who cannot tell whether the
     * asset is missing or the decoder is. */
    debugf("fig_video: '%s' not decoded on the host — the credits hold on "
           "the static card, which is the same path a ROM without the video "
           "takes.\n", dfs_path ? dfs_path : "(null)");
}

bool fig_video_update(FigVideo *mv, float dt)
{
    (void)mv; (void)dt;
    /* False on the first call: "the stream ended". pm_credits reads that as
     * PHASE_HOLD. */
    return false;
}

void fig_video_draw(FigVideo *mv, int screen_w, int screen_h)
{
    (void)mv; (void)screen_w; (void)screen_h;
    /* Nothing. update() never returned true, so a caller following the
     * contract never reaches here; one that does gets an untouched frame
     * rather than a grey rectangle pretending to be video. */
}

void fig_video_close(FigVideo *mv)
{
    if (mv) memset(mv, 0, sizeof *mv);
}
