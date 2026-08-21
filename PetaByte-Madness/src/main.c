// SPDX-License-Identifier: MPL-2.0
//
// PetaByte Madness — a first-person horror game on the Kiln engine.
//
// Patrick Horner climbed onto the MRI in an underwater lab and what he came
// out with is the veil: a filter he can raise to see what is actually down
// there. It costs him nothing on the hardware (it is a palette swap, not a
// full-screen blend — docs/VEIL_DESIGN.md) and it costs him plenty in the
// fiction, because while it is up, they can see him too.
//
// ── Slice 0 ────────────────────────────────────────────────────────────
// What this build is: the veil runtime, the four demons and their
// species rules, one generated corridor to walk them in, and the HUD that
// makes the veil's cost legible. That is the mechanic end to end — the
// thing worth proving before any art goes in.
//
// What it is NOT yet, in rough order of what should land next:
//   * The CI4 + TLUT bake (VEIL_DESIGN.md §8 step 1-2). Until it exists
//     pm_veil_bind_palette has no baked ramps to point at, so the visible
//     half of the veil is fog + far-plane + the phantom body gate.
//     pm_veil_ramp_build is the stand-in; see its comment.
//   * Demon animation. gltf_to_t3d drops the .glb animation channels
//     (they are rigid segmented hierarchies with no skin, and the importer
//     wants a skinned rig) — see PetaByte-Madness/README.md.
//   * Audio. veil.dsp was described in the design doc but not shipped in
//     the asset drop; both audio paths (baked stems crossfaded by
//     pm_veil_audio_t, live demon voices) are wired for in the design and
//     built for in this repo, and neither is connected here.
//   * Combat, damage, the LOACH, Horner himself as a viewmodel.
//
// main() owns the frame loop and nothing else. The veil is pm_veil, the
// demons are pm_demons, the HUD is pm_hud, and the world is a .map.

#include <libdragon.h>

#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_fpscam.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_map.h>
#include <kiln/kiln_actor.h>
#include <kiln/kiln_event.h>
#include <kiln/kiln_surface.h>
#include <kiln/kiln_audio.h>

#include <kiln/kiln_camera.h>
#include <kiln/kiln_widget.h>

#include "pm_types.h"
#include "pm_veil.h"
#include "pm_demons.h"
#include "pm_hud.h"
#include "pm_screens.h"
#include "pm_models.h"
#include "pm_lab.h"
#include "pm_actors.h"
#include "pm_fx.h"
#include "pm_sfx.h"
#include "pm_cine.h"
#include "pm_debug.h"

#define ACTOR_POOL_CAP 32

// Air drains slowly while the filter is down and fast while it is up. This
// is the second half of the veil's price (the first is being seen), and it
// is what stops the player from simply holding it up forever and freezing
// every hellhound in the level.
#define AIR_DRAIN_IDLE  0.60f
#define AIR_DRAIN_VEIL  4.50f

static KilnActor  g_actor_pool[ACTOR_POOL_CAP];
static KilnFpsCam g_cam;
static KilnMap    g_lab;

static void register_surfaces(void)
{
    // No SFX handles yet (audio is not wired), so footstep_sfx is -1
    // everywhere. Friction is real and already does something: the flooded
    // section drags, the plinths do not.
    const KilnSurfaceDef deck  = { .friction = 0.90f, .footstep_sfx = -1 };
    const KilnSurfaceDef water = { .friction = 1.15f, .footstep_sfx = -1 };
    const KilnSurfaceDef stone = { .friction = 0.95f, .footstep_sfx = -1 };
    kiln_surface_register(PM_SURF_DECK,  &deck);
    kiln_surface_register(PM_SURF_WATER, &water);
    kiln_surface_register(PM_SURF_STONE, &stone);
}

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    kiln_input_init();

    dfs_init(DFS_DEFAULT_LOCATION);
    // mkModel runs `mkasset -c 2` over each .t3dm. Level 2 is not linked in
    // by default — without this, asset_load cannot decompress and
    // t3d_model_load returns garbage rather than failing.
    asset_init_compression(2);

    // 32 kHz to match what pmDrone was baked at — mkN64Rom's audioRate
    // cross-checks the two at build time, because a mismatch is pitch and
    // time drift rather than silence, which is the kind of defect you only
    // notice after it has shipped.
    kiln_audio_init((KilnAudioConfig){ .sample_rate = 32000, .latency = 0.16f,
                                     .sfx_channels = 16, .music_channels = 8 });

    pm_sfx_init();

    register_surfaces();
    kiln_event_init();
    // One table, assembled from each module's own block. See pm_actors.h.
    pm_actors_init(g_actor_pool, ACTOR_POOL_CAP);
    pm_demons_load();

    // The map is the level: brushes for kiln_clip, faces for the 3D pass,
    // and point entities that become demons. Classnames must be bound
    // before the load or the entities are skipped.
    kiln_map_register_classname("info_imp",       PM_PROFILE_IMP);
    kiln_map_register_classname("info_hellhound", PM_PROFILE_HELLHOUND);
    kiln_map_register_classname("info_gargoyle",  PM_PROFILE_GARGOYLE);
    kiln_map_register_classname("info_overlord",  PM_PROFILE_OVERLORD);

    // Floor level at the map's start, plus the player's eye height. The map
    // is authored at this world's 64 units to the metre — it is a 16.3 m x
    // 5.6 m corridor with a 2.34 m ceiling — so the eye belongs at ~104,
    // not at the 52 this used to hardcode.
    const fm_vec3_t spawn = {{ 0.0f, pm_lab_eye_height(), -40.0f }};
    if (kiln_map_load(&g_lab, "rom:/maps/pm_lab.map") == 0) {
        kiln_clip_set_world(g_lab.brushes, g_lab.brush_count);
        for (uint16_t i = 0; i < g_lab.spawn_count; i++) {
            const KilnRoomSpawn *sp = &g_lab.spawns[i];
            kiln_actor_spawn(sp->profile_id, sp->pos, sp->yaw, &sp->dict);
        }
    } else {
        // A missing map is survivable — the player free-floats in an empty
        // scene rather than the ROM hanging — but "survivable" turned out to
        // mean "indistinguishable from working", and it stayed that way for
        // the whole life of PM_SCREEN_PLAY: the asset was named pm-lab-map,
        // this asked for pm_lab, and the load failed on every single boot.
        // PLAY had no collision at all, so the player fell forever and the
        // screen was black with a working HUD over it.
        //
        // A debugf did not help, because debugf goes to a flashcart nobody had
        // attached. What makes it visible now is the debug overlay's brush
        // count (pm_debug.c reads kiln_clip_world_count) — "clip 0" in red is
        // the one number that distinguishes "no collision installed" from every
        // other reason a first-person screen looks wrong. Still not fatal: the
        // rest of the game is unaffected and worth being able to run.
        debugf("pm: rom:/maps/pm_lab.map failed to load — PLAY will have no "
               "collision. flake.nix's pmLabMap `name` IS the filename.\n");
    }

    // One definition of the player's body, shared with PM_SCREEN_LAB. See
    // pm_lab_body — main.c used to keep a second copy here, written at a
    // different world scale.
    pm_lab_body(&g_cam);
    kiln_fpscam_snap(&g_cam, spawn, 0.0f, 0.0f);

    KilnScene scene;
    kiln_scene_init(&scene);
    // Near-black, cold. Everything the player sees with the veil down is
    // this plus the eye pinpricks.
    scene.clear_color = RGBA32(6, 7, 10, 255);

    PMVeil veil;
    // far_normal / far_veiled: the rebate. The corridor is 1000 units long,
    // so raising the filter visibly eats its far end — and buys back the
    // frame time the demons then spend.
    pm_veil_init(&veil, 1000.0f, 760.0f);
    // The baked cold/veiled TLUT pairs (flake.nix's pmVeilTextures). Loaded
    // once, here, because every one is a 288-byte ramp built by interpolating a
    // 64-byte pair — cheap, but not something to redo per screen. A ROM built
    // without them still runs: pm_veil_palette returns NULL and the filter is
    // its fog and far-plane halves, which is what shipped before the bake
    // existed. The debug overlay reports the count.
    pm_veil_palettes_init();

    PMPlayer player = {
        .health = 100, .max_health = 100,
        .air = 90.0f, .max_air = 90.0f, .seen = 0,
    };

    KilnCamera cam;
    kiln_camera_init(&cam);
    // kiln_camera_init deliberately leaves eye/look at the origin (see its
    // header comment) — fine for every screen that pushes CUTSCENE and
    // writes real keyframes before ever reading them back, but NORMAL mode
    // (LAB/PLAY) never calls kiln_camera_update itself (they run the fpscam
    // instead — see main.c's fps_active branch), so nothing else ever
    // replaces this zeroed pose. The first time the intro finishes and pops
    // CUTSCENE back to NORMAL for real (BEACH -> PLAY), eye==look produces a
    // zero-length view vector, which is exactly the NaN class kiln_camera.h's
    // own comment warns about — reproduced as a real VR4300 "floating point
    // (NaN, /0, or invalid op)" halt at t3d_viewport_attach. Snapping to the
    // same spawn pose the fpscam gets two lines above keeps this struct's
    // dead NORMAL-mode state non-degenerate instead of relying on it never
    // being read.
    kiln_camera_snap(&cam, spawn, 0.0f);

    PMApp app;
    pm_app_init(&app, &g_cam);

    // ── The lint ROM ───────────────────────────────────────────────────
    // `PM_CINE_LINT=1` builds a ROM whose whole job is the camera report, so
    // it runs the validator here — before the loop, after every asset system
    // is up, because each shot's setup() is what fills its keys (the flyover's
    // fourteen are built by flyover_build_keys and are all zeroes until then).
    if (pm_cine_lint_mode()) pm_cine_lint_run();

    uint32_t last_ticks = get_ticks();

    for (;;) {
        uint32_t now = get_ticks();
        float dt = (float)TICKS_DISTANCE(last_ticks, now) / TICKS_PER_SECOND;
        last_ticks = now;
        // Clamp the first frame and any hitch: a multi-second dt walks a
        // demon through a wall and drains the whole air bar in one step.
        if (dt > 0.1f) dt = 0.1f;
        // Hit-stop freezes the world but not the effects selling the hit:
        // pm_fx runs on the real dt (inside pm_screens_update), gameplay
        // and animation run on this one.
        const float game_dt = dt * pm_fx_time_scale();

        kiln_input_update();
        pm_debug_input(kiln_input_get(0));
        // The cinematic transport, and the pad it takes with it. pm_debug_input
        // runs FIRST so its layer chords still work while the transport is
        // armed — the two want to be used together, and the overlay's layers
        // are what a paused shot is being inspected with.
        pm_cine_input(kiln_input_get(0));
        // The widget layer's sway and selection overshoot hang off one
        // clock; nothing on a menu screen holds still without it.
        kiln_widget_tick(dt);
        // While the transport is armed it OWNS the pad, and the screen machine
        // sees a dead controller. Not swallowing it means START skips the very
        // shot being studied (pm_screens.c's intro_update) and D-up/D-down walk
        // a menu cursor underneath the timeline.
        static const KilnInput DEAD_PAD;
        const KilnInput *in = pm_cine_grabs_input() ? &DEAD_PAD
                                                   : kiln_input_get(0);

        // ── Pump the RSP mixer FIRST, before any geometry is queued ──────
        // mixer_poll mixes on the RSP, and so does Tiny3D. This call used to
        // sit at the BOTTOM of the loop, "after everything that could have
        // started a sound" — which meant it queued behind the entire frame's
        // 3D command list and had to wait for it.
        //
        // That was survivable while the flyover drew ~1,900 triangles. Once
        // it drew a sky dome, denser terrain, a textured sea and ten palms,
        // the mixer missed a buffer roughly once per buffer cycle: recording
        // the console output found the audio sitting at exactly digital zero
        // for 0.4-10 ms every 0.16 s — the configured latency — in BOTH the
        // XM64 and the streamed wav64 path, because both go through this
        // same mixer.
        //
        // Pumped first, the mixer takes an idle RSP and the geometry queues
        // behind IT. Video has a whole frame of slack; audio has none. The
        // cost is that a sound triggered this frame starts one frame later,
        // which is 16 ms.
        kiln_audio_update();

        if (pm_cine_lint_mode()) {
            // Nothing else runs. The report IS the ROM, and letting the game
            // play underneath it would only give the capture something to be
            // confused by.
            kiln_scene_update(&scene);
            kiln_frame_begin();
            kiln_scene_begin(&scene);
            kiln_gui_begin();
            pm_cine_lint_draw(PM_SCREEN_W, PM_SCREEN_H);
            kiln_gui_end();
            kiln_frame_end();
            continue;
        }

        // Which screen is up, and what drives the camera on it. Everything
        // before PLAY is a scripted shot; pm_screens owns that.
        const PMScreen screen =
            pm_screens_update(&app, in, &cam, &scene, &veil, dt);
        const int playing = (screen == PM_SCREEN_PLAY);

        // Entering PLAY, from the intro or straight off the file screen.
        // The lab installed its OWN clip world and dropped it on the way
        // out, so the corridor's has to be put back — otherwise the player
        // arrives in PLAY with nothing to stand on.
        //
        // pm_lab_body goes with it, and not only for the AABB: the player
        // can leave PM_SCREEN_LAB mid-fall (START is live every frame), and
        // kiln_fpscam_snap sets a pose, not a velocity. Without the re-init
        // that downward velocity carries across the transition and drives
        // him through the corridor floor on the first frame of PLAY.
        static PMScreen prev_screen = PM_SCREEN_BOOT;
        if (playing && prev_screen != PM_SCREEN_PLAY) {
            kiln_clip_set_world(g_lab.brushes, g_lab.brush_count);
            pm_lab_body(&g_cam);
            kiln_fpscam_snap(&g_cam, spawn, 0.0f, 0.0f);
        }
        prev_screen = screen;

        // The lab is first person too, so it and PLAY share the camera;
        // every other screen is on the scripted one.
        const int fps_active = playing || (screen == PM_SCREEN_LAB);

        // ── Player ─────────────────────────────────────────────────────
        if (playing) kiln_fpscam_update(&g_cam, in, game_dt);

        // ── Veil ───────────────────────────────────────────────────────
        // Z is the toggle: held up, released down. A press-to-latch toggle
        // makes it too easy to leave up and forget, and the whole design
        // is about managing when it is up.
        pm_veil_set(&veil, playing && (in->buttons & KILN_BTN_Z) != 0);
        // An overlord's halo overrides that. Queried before the update so
        // the force lands on the same frame the player walks into it.
        pm_veil_force(&veil, playing ? pm_demons_veil_force_at(g_cam.pos) : 0.0f);
#if defined(KILN_DEBUG) && defined(PM_VEIL_FORCE)
        // `PM_VEIL_FORCE=1` pins the veil fully on, everywhere, for capture.
        //
        // The palette swap only shows on a CI4-textured model, and the only one
        // is the centaur — who appears on the attract reel and the beach, where
        // the veil never runs (main.c applies it in PLAY only, and the demons
        // in PLAY are still untextured). So without this the effect is
        // unphotographable, and "unphotographable" is how the veil's whole TLUT
        // half went unnoticed as dead code for as long as it did.
        //
        // Debug ROMs only, and a separate flag from PM_JUMP so a jump ROM can
        // be captured either way:
        //   nix build .#pm-veil-beach && ./dev shot pm-veil-beach up.png 8
        pm_veil_force(&veil, 1.0f);
#endif
        pm_veil_update(&veil, dt);
        // Writes fog + the rebated far plane into the scene, so it has to
        // come before kiln_scene_update rebuilds the projection.
        // ONLY in PLAY. pm_veil_apply_scene writes scene->far_z (1000 at
        // t=0) and enables fog; running it on the title, the attract reel
        // and every cutscene clipped everything past ~15 m — including the
        // whole island, whose flyover camera sits thousands of units out.
        // The veil is a gameplay mechanic and has no business setting the
        // frustum for a menu.
        if (playing) pm_veil_apply_scene(&veil, &scene);

        // ── Demons ─────────────────────────────────────────────────────
        if (playing) {
            pm_demons_bind(&veil, g_cam.pos);
            // Events land before the actors' own updates, per kiln_event.h.
            kiln_event_process(game_dt);
            kiln_actor_update_all(game_dt);
            player.seen = (uint8_t)pm_demons_player_seen();

            // ── Air ────────────────────────────────────────────────────
            player.air -= (AIR_DRAIN_IDLE
                           + (AIR_DRAIN_VEIL - AIR_DRAIN_IDLE) * veil.t) * dt;
            if (player.air < 0.0f) {
                player.air = 0.0f;
                player.health -= (int32_t)(24.0f * dt);
                if (player.health < 0) player.health = 0;
            }

        }
        // AFTER the director has advanced (pm_screens_update ran it) and
        // BEFORE the camera lands in the scene, which is what the free-fly
        // then overwrites. Real dt, not the transported one: the viewer's
        // camera has to keep moving while the shot it is inspecting is frozen.
        pm_cine_frame(dt);

        if (screen == PM_SCREEN_BOOT) {
            // The splash already wrote the scene's camera in
            // pm_screens_update; anything else here would overwrite it.
        } else if (fps_active) {
            kiln_fpscam_apply(&g_cam, &scene);
        } else {
            // The scripted camera owns the scene on every other screen.
            kiln_camera_apply(&cam, &scene);
        }
        // Detach: the shot keeps running and keeps writing its own pose, this
        // just stops that pose being what the frame is built from. No-op unless
        // the transport is armed and Z has been pressed, so it needs no guard.
        (void)pm_cine_override_camera(&scene);
        // After the camera, before the matrices: the shake belongs in the
        // view rather than fighting the damper that produced it.
        pm_fx_apply_camera(&scene);
        kiln_scene_update(&scene);

        // ── Draw ───────────────────────────────────────────────────────
        kiln_frame_begin();
        kiln_scene_begin(&scene);

        if (playing) {
            kiln_map_draw(&g_lab);
            // Demon bodies gate themselves on the veil inside their draw
            // callback — that gate is the phantom rule and it is why a
            // corridor can hold a dozen of them.
            kiln_actor_draw_all();
        } else {
            pm_screens_draw3d(&app);
        }

        kiln_gui_begin();
        if (playing) {
            // Eyes first: they belong to the world, not the HUD, and
            // should sit under the panels if they ever overlap.
            pm_demons_draw_eyes(&scene);
            pm_hud_draw(&player, &veil, PM_SCREEN_W, PM_SCREEN_H);
            // Vignette last, and only if there is fill rate for it — the
            // one optional component in the whole effect.
            pm_veil_draw_vignette(&veil, PM_SCREEN_W, PM_SCREEN_H);
        }
        // Menus and the fade draw over everything, on every screen.
        pm_screens_draw2d(&app, PM_SCREEN_W, PM_SCREEN_H);
        // The spatial overlay — brushes, actors, camera paths, anchors —
        // before the text panel, so the panel is never obscured by the lines
        // it is reporting counts for. Both compile to nothing without
        // KILN_DEBUG; see pm_debug.h.
        pm_debug_draw3d(&app, &scene, PM_SCREEN_W, PM_SCREEN_H);
        // The shot camera's own gizmo and frustum, drawn from wherever the
        // free-fly is standing. Nothing unless detached.
        pm_cine_draw3d(&scene, PM_SCREEN_W, PM_SCREEN_H);
        // And the debug readout over THAT, in the debug ROM only — it is
        // describing what is underneath it, including the fade.
        pm_debug_draw(&app, &scene, fps_active ? &g_cam : NULL, &veil,
                      dt, PM_SCREEN_W, PM_SCREEN_H);
        // The timeline, the cue trace and — when detached — the PMCamKey pose
        // readout. Last, because it is the thing being read.
        pm_cine_draw(PM_SCREEN_W, PM_SCREEN_H);
        kiln_gui_end();

        kiln_frame_end();
    }
}
