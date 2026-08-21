// SPDX-License-Identifier: MPL-2.0
//
// pm_demo.h — scripted camera shots over live scenes.
//
// One director, four customers: the title screen's drone flyover, the
// attract reel that cycles when the title is left alone, the submarine
// pan after the transformation, and the beach where the reveal happens.
// Each is the same thing — a keyframed eye/look pair driving
// KILN_CAM_CUTSCENE while real geometry draws underneath — so each is a
// PMDemoShot rather than four hand-rolled camera loops.
//
// Modelled on examples/cinematic-demo's 8-keyframe / 60-second script; the
// bracketing-keyframe search is the same shape. What is new here is that
// shots are DATA in a table with their own setup/draw hooks, because these
// shots have to be started, cycled, interrupted and skipped by a screen
// state machine rather than run once from main().
//
// ── These are live scenes, not video ───────────────────────────────────
// A shot's `draw` runs inside the 3D pass and puts real models on screen;
// where a shot wants actors, its `setup` spawns them and the normal
// kiln_actor_update_all / kiln_actor_draw_all runs underneath. That is the
// whole point of the attract reel: the background of the menu is the
// actual game, not a pre-rendered movie the console plays back.
//
// ── The flyover is the menu's, and only the menu's ─────────────────────
// The drone flyover belongs to the title screen and the attract reel. The
// submarine pan is a DIFFERENT shot doing a different job: low, rising,
// staying with the boat. Do not let them converge — an intro that opens
// on the same sweeping aerial the menu just used reads as the menu again,
// and the arrival stops being an arrival.

#ifndef PM_DEMO_H
#define PM_DEMO_H

#include <t3d/t3dmath.h>
#include <kiln/kiln_camera.h>
#include <kiln/kiln_engine.h>

// PMCamKey and the Catmull-Rom through a table of them. Split into their own
// header so the runtime, the debug overlay and the static validator all fly the
// SAME curve rather than three that agree by inspection — pm_camkey.h says why
// that is load-bearing.
#include "pm_camkey.h"

typedef struct {
    const char     *name;
    float           duration;   /**< seconds; the shot ends here          */
    const PMCamKey *keys;
    uint8_t         key_count;

    /** Called once when the shot starts. Spawn actors, set the veil,
     *  preload models. May be NULL. */
    void (*setup)(void);
    /** Called once when the shot ends or is cut short. Despawn whatever
     *  `setup` spawned. May be NULL. */
    void (*teardown)(void);
    /** Called every frame inside the 3D pass. May be NULL. */
    void (*draw)(float elapsed);
    /** Called every frame before the camera update, for anything the shot
     *  animates itself. May be NULL. */
    void (*update)(float elapsed, float dt);

    /** This shot's frustum. A shot that frames a 200 m island and a shot
     *  that frames a face cannot share one near/far pair — the island needs
     *  tens of thousands of units of depth and the face needs tight
     *  precision a few units from the lens. Leaving either 0 falls back to
     *  PM_SHOT_NEAR_Z / PM_SHOT_FAR_Z below, NOT to the scene's current
     *  values — see pm_demo_apply_frustum for why that distinction cost
     *  six shots their geometry. */
    float near_z, far_z;

    /** 1 if this shot is outdoors on the island at night. The director
     *  installs pm_env's moonlit rig (lights, fog, clear colour) on the
     *  scene when it applies the frustum — which is before
     *  kiln_scene_begin uploads the lights, and therefore the only place a
     *  shot CAN choose its lighting. A shot's draw callback runs after the
     *  upload and is far too late.
     *
     *  Data rather than a call in `setup` for the same reason near_z/far_z
     *  are data: the reel mixes interiors and exteriors, and a shot that
     *  forgets to undo the previous one's lighting is the bug this
     *  prevents. */
    uint8_t exterior;
} PMDemoShot;

/** The frustum a shot gets when it declares none of its own.
 *
 *  This world runs at 64 units to the metre (pm_lab.h states the
 *  convention; gltf_to_t3d's --base-scale=64 enforces it on every model),
 *  so these are 0.16 m and 62.5 m. That covers every interior shot in the
 *  game — the lab is 631 units across — and a shot that needs the horizon
 *  says so explicitly rather than relying on the default.
 *
 *  kiln_scene_init's own default is 10/200, which is 3 m of draw distance
 *  here. Do not let a shot inherit it. */
#define PM_SHOT_NEAR_Z   10.0f
#define PM_SHOT_FAR_Z  4000.0f

/** The opening cinematic: Horner in the lab, then into his head. Plays
 *  once on a new profile, and is NOT in the attract reel — it ends on the
 *  exact pose pm_lab_enter expects, which only means anything there. */
extern const PMDemoShot pm_demo_lab_cine;

/** The attract reel, in order. Exposed so the title screen can start the
 *  flyover directly (it is shot 0) without knowing the rest. */
extern const PMDemoShot pm_demo_reel[];
extern const int        pm_demo_reel_count;

/** Start a shot. `loop` keeps it running past its duration (what the title
 *  screen wants of the flyover); otherwise it ends and pm_demo_done
 *  reports 1. Calls the previous shot's teardown first. */
void pm_demo_play(const PMDemoShot *shot, int loop);

/** Stop the current shot and run its teardown. Safe with nothing playing. */
void pm_demo_stop(void);

/** Advance the shot clock and run the shot's own update. */
void pm_demo_update(float dt);

/** 1 once a non-looping shot has passed its duration. */
int pm_demo_done(void);

/** Seconds since the shot started (wrapped, when looping). */
float pm_demo_elapsed(void);

/** 1 if the current shot was started with `loop`. Exposed because it changes
 *  the CURVE, not just when the shot ends: pm_camkey_sample wraps a looping
 *  shot's neighbour keys so the seam is as smooth as anywhere else, and clamps
 *  a one-shot's instead. Anything re-deriving the flown path — pm_debug's
 *  overlay, pm_cine's seek, the validator — has to be told which. */
int pm_demo_looping(void);

/** Apply the current shot's near/far to the scene, if it declared any.
 *  Call before kiln_scene_update. */
void pm_demo_apply_frustum(KilnScene *scene);

/** The scene the director is currently drawing into, or NULL before the
 *  first pm_demo_apply_frustum. For shot draw callbacks that need the eye
 *  position — the sky dome centres on it — without the callback signature
 *  growing a parameter every other shot would ignore. */
const KilnScene *pm_demo_scene(void);

/** Write the interpolated eye/look into `cam` as a CUTSCENE pose. The
 *  caller still owns kiln_camera_update / _apply, same division
 *  examples/cinematic-demo uses. */
void pm_demo_apply(KilnCamera *cam);

/** Draw the current shot. Call inside the 3D pass. */
void pm_demo_draw(void);

/** The shot currently playing, or NULL. Read-only.
 *
 *  Exposed for pm_debug's spatial overlay, which draws the shot's `keys` as a
 *  camera path (kiln_dd_path) with a sightline from each eye key to its look
 *  target. That is worth being able to do for ANY shot rather than for one
 *  hardcoded in the overlay: every keyframed camera in this game is a PMCamKey
 *  array, so one accessor covers LAB_CINE, the whole attract reel, the intake,
 *  the sub and the beach.
 *
 *  It also makes a specific, recurring defect visible. pm_demo_apply
 *  interpolates with a Catmull-Rom spline whose tangent at a key comes from
 *  that key's TWO neighbours, so a large gap next to a small one drags the
 *  curve past the small one — the overshoot the intake's own keys carry three
 *  hand-inserted midpoints to suppress. Drawn, that is a curve visibly bulging
 *  through a wall; as a table of eye coordinates it is invisible, which is why
 *  it was found by flying the camera and watching. */
const PMDemoShot *pm_demo_current(void);

#endif // PM_DEMO_H
