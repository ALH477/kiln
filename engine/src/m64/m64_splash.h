/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_splash.h — the M64 boot splash.
 *
 * A parody of the Nintendo 64's boot: a chunky 3D wordmark assembles,
 * spins to rest, a publisher line fades up under it, and a jingle
 * resolves on the downbeat. Then it gets out of the way.
 *
 * ── Why this is in the engine ──────────────────────────────────────────
 * It is a publisher mark, not a game's title screen. Every ROM built on
 * this engine should be able to open with it by calling four functions,
 * and none of them should have to re-time the animation or re-derive when
 * the flash lands relative to the music. PetaByte Madness and Ganja Goblin
 * want the identical sequence with different things after it.
 *
 * ── Assets are the GAME's, not the engine's ────────────────────────────
 * The engine ships no files. The logo model and the jingle are built at
 * the flake level (tools/blender/m64_logo.py, dsp/m64_jingle.dsp) and a
 * ROM adds them to its own `assets` list; m64_splash_init takes what it
 * needs as handles. That keeps libm64 a library — a ROM that does not
 * want the splash pays nothing, and one that does is not forced to accept
 * an asset layout it did not choose.
 *
 * Both are optional. A missing model draws the wordmark's silhouette as
 * plain rectangles instead, and a missing jingle is silent; the timing
 * does not change either way, so a build with no assets still shows the
 * right sequence at the right length.
 *
 * ── The timing is the design ───────────────────────────────────────────
 * The N64's boot works because the picture and the sound resolve on the
 * same frame. Here: the wordmark's pieces fly in over the first second,
 * it settles by 1.25 s, and the jingle's chord and the screen flash both
 * land exactly there. Move one, move all three — they are one beat, and
 * m64_splash.c keeps them as one constant for that reason.
 *
 *   0.00  black, the pieces begin to arrive
 *   1.25  settled, chord resolves, flash    <- the beat
 *   1.60  publisher line fades up
 *   3.20  begins to fade out
 *   3.80  done
 *
 * ── Usage ──────────────────────────────────────────────────────────────
 *   m64_splash_init(model, jingle_sfx, "Made by DeMoD LLC");
 *   ... each frame, before the game's own update:
 *   if (!m64_splash_done()) {
 *       m64_splash_update(dt);
 *       m64_splash_apply(&scene);        // owns the camera while it runs
 *       ... m64_scene_update / begin ...
 *       m64_splash_draw3d();
 *       m64_gui_begin(); m64_splash_draw2d(w, h); m64_gui_end();
 *   }
 *
 * Any button skips it. A boot animation that cannot be skipped is the
 * thing everyone remembers hating.
 */
#ifndef M64_SPLASH_H
#define M64_SPLASH_H

#include <t3d/t3dmodel.h>

#include "m64_engine.h"
#include "m64_input.h"

/** Bind the splash's assets. `model` may be NULL (a rectangle fallback is
 *  drawn instead) and `jingle_sfx` may be -1 (silent). `line` is the
 *  publisher text; NULL draws none. Nothing is copied — both the model and
 *  the string must outlive the splash. */
void m64_splash_init(T3DModel *model, int jingle_sfx, const char *line);

/** Advance. Reads `in` only to decide whether the player skipped; pass
 *  NULL to make it unskippable (don't). */
void m64_splash_update(float dt, const M64Input *in);

/** 1 once the splash has finished or been skipped. */
int m64_splash_done(void);

/** Write the splash's camera into `scene`. Call before m64_scene_update. */
void m64_splash_apply(M64Scene *scene);

/** Draw the logo. Call inside the 3D pass. */
void m64_splash_draw3d(void);

/** Draw the publisher line and the fades. Call inside the GUI pass. */
void m64_splash_draw2d(int screen_w, int screen_h);

#endif /* M64_SPLASH_H */
