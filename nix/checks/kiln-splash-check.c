/* SPDX-License-Identifier: MIT
 *
 * Drives the REAL kiln_splash_init/update/apply/draw3d/draw2d sequence —
 * the engine's actual boot splash, not a stand-in for it — against a real
 * .t3dm converted from tools/blender/kiln_logo.py's own output, and
 * captures the settled frame.
 *
 * ── Structural assertions first, because the flame is a separate object ──
 * kiln_splash_draw3d looks up "kiln", "flame" and "plate" by name and gives
 * the flame its own transform so it can flicker independently of the body's
 * settle. If a future edit to kiln_logo.py renames or drops one of those
 * objects, kiln_splash quietly falls back to drawing the whole model as one
 * rigid piece — correct behaviour for an older/foreign model, but exactly
 * wrong for THIS model, and a pixel diff downstream would report it as
 * "the frame changed" with no hint why. Asserting the three lookups here
 * catches the specific failure at the specific place it would occur.
 */
#include <t3d/t3dmodel.h>
#include <kiln_engine.h>
#include <kiln_gui.h>
#include <kiln_input.h>
#include <kiln_splash.h>
#include <kiln_host.h>
#include <libdragon.h>

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* Link-satisfying only: this check passes jingle_sfx = -1, so kiln_splash
 * never calls into the real mixer. */
int kiln_sfx_play(int sfx_handle, int channel, int priority)
{ (void)sfx_handle; (void)channel; (void)priority; return -1; }
void kiln_sfx_stop(int channel) { (void)channel; }

int main(int argc, char **argv)
{
    const char *model_path = argc > 1 ? argv[1] : "kiln_logo.t3dm";
    const char *png = argc > 2 ? argv[2] : "splash.png";

    kiln_engine_init(RESOLUTION_320x240);
    kiln_gui_init();

    T3DModel *model = t3d_model_load(model_path);
    CHECK(model != NULL, "t3d_model_load('%s') returned NULL", model_path);
    if (!model) return 1;

    T3DObject *kiln_obj = t3d_model_get_object(model, "kiln");
    T3DObject *flame_obj = t3d_model_get_object(model, "flame");
    T3DObject *plate_obj = t3d_model_get_object(model, "plate");
    CHECK(kiln_obj != NULL, "no object named \"kiln\" in kiln_logo.t3dm");
    CHECK(flame_obj != NULL, "no object named \"flame\" in kiln_logo.t3dm — "
          "kiln_splash's flame flicker needs it to draw the flame at all");
    CHECK(plate_obj != NULL, "no object named \"plate\" in kiln_logo.t3dm");

    kiln_splash_init(model, -1, "Kiln Engine - MIT Licensed");

    /* Fixed dt, no wall-clock: the same determinism rule every asset in this
     * tree is held to (see nix/assets.nix's header). Run to the settled,
     * holding portion of the sequence — after the beat's flash has decayed
     * and before the fade-out starts — where the kiln, the flame and the lit
     * publisher line are all on screen together. */
    const float dt = 1.0f / 60.0f;
    const int settle_frame = 126;   /* ~2.10 s: BEAT=1.25, T_FADE_OUT=3.20 */
    int frame = 0;

    while (!kiln_splash_done() && frame <= settle_frame) {
        kiln_splash_update(dt, NULL);

        KilnScene scene;
        kiln_scene_init(&scene);
        kiln_splash_apply(&scene);
        kiln_scene_update(&scene);

        kiln_frame_begin();
          kiln_scene_begin(&scene);
            kiln_splash_draw3d();
          kiln_gui_begin();
            kiln_splash_draw2d(320, 240);
          kiln_gui_end();
        kiln_frame_end();

        frame++;
    }
    CHECK(!kiln_splash_done(), "splash finished before frame %d (got %d) — "
          "T_END must have shrunk", settle_frame, frame);

    const KilnHostT3DCounters *t = kiln_host_t3d_counters();
    printf("  frame %d: tris submitted %u drawn %u\n", frame - 1,
           t->tris_submitted, t->tris_drawn);
    CHECK(t->tris_submitted > 0, "no geometry submitted by the settled frame");
    CHECK(t->tris_drawn > 0, "geometry submitted but nothing rasterised");

    kiln_host_stats(stdout, 5);
    CHECK(kiln_host_capture(png) == 0, "could not write %s", png);

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("the real boot splash rendered the kiln, its flame and the lit "
           "publisher line\n");
    return 0;
}
