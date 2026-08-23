/* SPDX-License-Identifier: MIT
 *
 * kiln_splash.h — the Kiln boot splash.
 *
 * A parody of the Nintendo 64's boot, with the engine's own name for a
 * subject: a classical brick kiln assembles, spins to rest with a flame
 * flickering at its doorway, a publisher line lit by that same flame fades
 * up under it, and a jingle resolves on the downbeat. Then it gets out of
 * the way.
 *
 * ── Why this is in the engine ──────────────────────────────────────────
 * It is a publisher mark, not a game's title screen. Every ROM built on
 * this engine should be able to open with it by calling four functions,
 * and none of them should have to re-time the animation or re-derive when
 * the flash lands relative to the music. Every game built on this engine so
 * far wants the identical sequence with different things after it.
 *
 * ── Assets are the GAME's, not the engine's ────────────────────────────
 * The engine ships no files. The kiln model and the jingle are built at
 * the flake level (tools/blender/kiln_logo.py, dsp/kiln_jingle.dsp) and a
 * ROM adds them to its own `assets` list; kiln_splash_init takes what it
 * needs as handles. That keeps libkiln a library — a ROM that does not
 * want the splash pays nothing, and one that does is not forced to accept
 * an asset layout it did not choose.
 *
 * Both are optional. A missing model draws a silhouette as plain
 * rectangles instead, and a missing jingle is silent; the timing does not
 * change either way, so a build with no assets still shows the right
 * sequence at the right length.
 *
 * ── The flame moves separately from the body ───────────────────────────
 * kiln_logo.py exports three named objects — "kiln" (the furnace body,
 * the piece that spins to rest), "flame" (a small cluster of licks at the
 * doorway) and "plate" (the accent bar under both). kiln_splash_draw3d
 * looks the first two up by name and gives the flame its own transform, so
 * it can flicker in place independently of the body's settle instead of
 * being baked into the mesh — the RSP has no per-object vertex animation
 * on this hardware, so an independent OBJECT is what stands in for one. A
 * model that does not carry these names — an older build, or a caller's
 * own custom logo — draws as a single rigid piece, exactly as this module
 * always has; the lookup missing is not an error.
 *
 * The same flicker phase that flexes the flame's scale also warms the
 * publisher line's colour in the 2D pass (see kiln_splash_draw2d) — the
 * closest this engine's 2D pass can come to "light from the fire hits the
 * text" when the 2D pass has no lighting at all.
 *
 * ── The timing is the design ───────────────────────────────────────────
 * The N64's boot works because the picture and the sound resolve on the
 * same frame. Here: the wordmark's pieces fly in over the first second,
 * it settles by 1.25 s, and the jingle's chord and the screen flash both
 * land exactly there. Move one, move all three — they are one beat, and
 * kiln_splash.c keeps them as one constant for that reason.
 *
 *   0.00  black, the pieces begin to arrive
 *   1.25  settled, chord resolves, flash    <- the beat
 *   1.60  publisher line fades up
 *   3.20  begins to fade out
 *   3.80  done
 *
 * ── Usage ──────────────────────────────────────────────────────────────
 *   kiln_splash_init(model, jingle_sfx, "Kiln Engine - MIT Licensed");
 *   ... each frame, before the game's own update:
 *   if (!kiln_splash_done()) {
 *       kiln_splash_update(dt, &input);
 *       kiln_splash_apply(&scene);        // owns the camera while it runs
 *       ... kiln_scene_update / begin ...
 *       kiln_splash_draw3d();
 *       kiln_gui_begin(); kiln_splash_draw2d(w, h); kiln_gui_end();
 *   }
 *
 * Any button skips it. A boot animation that cannot be skipped is the
 * thing everyone remembers hating.
 */
#ifndef KILN_SPLASH_H
#define KILN_SPLASH_H

#include <t3d/t3dmodel.h>

#include "kiln_engine.h"
#include "kiln_input.h"

/** Bind the splash's assets. `model` may be NULL (a rectangle fallback is
 *  drawn instead) and `jingle_sfx` may be -1 (silent). `line` is the
 *  publisher text; NULL draws none. Nothing is copied — both the model and
 *  the string must outlive the splash. */
void kiln_splash_init(T3DModel *model, int jingle_sfx, const char *line);

/** Advance. Reads `in` only to decide whether the player skipped; pass
 *  NULL to make it unskippable (don't). */
void kiln_splash_update(float dt, const KilnInput *in);

/** 1 once the splash has finished or been skipped. */
int kiln_splash_done(void);

/** Write the splash's camera into `scene`. Call before kiln_scene_update. */
void kiln_splash_apply(KilnScene *scene);

/** Draw the logo. Call inside the 3D pass. */
void kiln_splash_draw3d(void);

/** Draw the publisher line and the fades. Call inside the GUI pass. */
void kiln_splash_draw2d(int screen_w, int screen_h);

#endif /* KILN_SPLASH_H */
