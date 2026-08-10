// SPDX-License-Identifier: MPL-2.0
//
// PetaByte Madness — a first-person horror game on the M64 engine.
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

#include <m64/m64_engine.h>
#include <m64/m64_gui.h>
#include <m64/m64_input.h>
#include <m64/m64_fpscam.h>
#include <m64/m64_clip.h>
#include <m64/m64_map.h>
#include <m64/m64_actor.h>
#include <m64/m64_event.h>
#include <m64/m64_surface.h>
#include <m64/m64_audio.h>

#include <m64/m64_camera.h>
#include <m64/m64_widget.h>

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
#include "pm_debug.h"

#define ACTOR_POOL_CAP 32

// Air drains slowly while the filter is down and fast while it is up. This
// is the second half of the veil's price (the first is being seen), and it
// is what stops the player from simply holding it up forever and freezing
// every hellhound in the level.
#define AIR_DRAIN_IDLE  0.60f
#define AIR_DRAIN_VEIL  4.50f

static M64Actor  g_actor_pool[ACTOR_POOL_CAP];
static M64FpsCam g_cam;
static M64Map    g_lab;

static void register_surfaces(void)
{
    // No SFX handles yet (audio is not wired), so footstep_sfx is -1
    // everywhere. Friction is real and already does something: the flooded
    // section drags, the plinths do not.
    const M64SurfaceDef deck  = { .friction = 0.90f, .footstep_sfx = -1 };
    const M64SurfaceDef water = { .friction = 1.15f, .footstep_sfx = -1 };
    const M64SurfaceDef stone = { .friction = 0.95f, .footstep_sfx = -1 };
    m64_surface_register(PM_SURF_DECK,  &deck);
    m64_surface_register(PM_SURF_WATER, &water);
    m64_surface_register(PM_SURF_STONE, &stone);
}

int main(void)
{
    m64_engine_init(RESOLUTION_320x240);
    joypad_init();
    m64_input_init();

    dfs_init(DFS_DEFAULT_LOCATION);
    // mkModel runs `mkasset -c 2` over each .t3dm. Level 2 is not linked in
    // by default — without this, asset_load cannot decompress and
    // t3d_model_load returns garbage rather than failing.
    asset_init_compression(2);

    // 32 kHz to match what pmDrone was baked at — mkN64Rom's audioRate
    // cross-checks the two at build time, because a mismatch is pitch and
    // time drift rather than silence, which is the kind of defect you only
    // notice after it has shipped.
    m64_audio_init((M64AudioConfig){ .sample_rate = 32000, .latency = 0.16f,
                                     .sfx_channels = 16, .music_channels = 8 });

    pm_sfx_init();

    register_surfaces();
    m64_event_init();
    // One table, assembled from each module's own block. See pm_actors.h.
    pm_actors_init(g_actor_pool, ACTOR_POOL_CAP);
    pm_demons_load();

    // The map is the level: brushes for m64_clip, faces for the 3D pass,
    // and point entities that become demons. Classnames must be bound
    // before the load or the entities are skipped.
    m64_map_register_classname("info_imp",       PM_PROFILE_IMP);
    m64_map_register_classname("info_hellhound", PM_PROFILE_HELLHOUND);
    m64_map_register_classname("info_gargoyle",  PM_PROFILE_GARGOYLE);
    m64_map_register_classname("info_overlord",  PM_PROFILE_OVERLORD);

    // Floor level at the map's start, plus the player's eye height. The map
    // is authored at this world's 64 units to the metre — it is a 16.3 m x
    // 5.6 m corridor with a 2.34 m ceiling — so the eye belongs at ~104,
    // not at the 52 this used to hardcode.
    const fm_vec3_t spawn = {{ 0.0f, pm_lab_eye_height(), -40.0f }};
    if (m64_map_load(&g_lab, "rom:/maps/pm_lab.map") == 0) {
        m64_clip_set_world(g_lab.brushes, g_lab.brush_count);
        for (uint16_t i = 0; i < g_lab.spawn_count; i++) {
            const M64RoomSpawn *sp = &g_lab.spawns[i];
            m64_actor_spawn(sp->profile_id, sp->pos, sp->yaw, &sp->dict);
        }
    } else {
        // A missing map is survivable: the player free-floats in an empty
        // scene rather than the ROM hanging on a black screen, which is
        // the difference between "the DFS path is wrong" and "the game is
        // broken" when this comes up on hardware.
        debugf("pm: rom:/maps/pm_lab.map failed to load\n");
    }

    // One definition of the player's body, shared with PM_SCREEN_LAB. See
    // pm_lab_body — main.c used to keep a second copy here, written at a
    // different world scale.
    pm_lab_body(&g_cam);
    m64_fpscam_snap(&g_cam, spawn, 0.0f, 0.0f);

    M64Scene scene;
    m64_scene_init(&scene);
    // Near-black, cold. Everything the player sees with the veil down is
    // this plus the eye pinpricks.
    scene.clear_color = RGBA32(6, 7, 10, 255);

    PMVeil veil;
    // far_normal / far_veiled: the rebate. The corridor is 1000 units long,
    // so raising the filter visibly eats its far end — and buys back the
    // frame time the demons then spend.
    pm_veil_init(&veil, 1000.0f, 760.0f);

    PMPlayer player = {
        .health = 100, .max_health = 100,
        .air = 90.0f, .max_air = 90.0f, .seen = 0,
    };

    M64Camera cam;
    m64_camera_init(&cam);

    PMApp app;
    pm_app_init(&app, &g_cam);

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

        m64_input_update();
        pm_debug_input(m64_input_get(0));
        // The widget layer's sway and selection overshoot hang off one
        // clock; nothing on a menu screen holds still without it.
        m64_widget_tick(dt);
        const M64Input *in = m64_input_get(0);

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
        m64_audio_update();

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
        // m64_fpscam_snap sets a pose, not a velocity. Without the re-init
        // that downward velocity carries across the transition and drives
        // him through the corridor floor on the first frame of PLAY.
        static PMScreen prev_screen = PM_SCREEN_BOOT;
        if (playing && prev_screen != PM_SCREEN_PLAY) {
            m64_clip_set_world(g_lab.brushes, g_lab.brush_count);
            pm_lab_body(&g_cam);
            m64_fpscam_snap(&g_cam, spawn, 0.0f, 0.0f);
        }
        prev_screen = screen;

        // The lab is first person too, so it and PLAY share the camera;
        // every other screen is on the scripted one.
        const int fps_active = playing || (screen == PM_SCREEN_LAB);

        // ── Player ─────────────────────────────────────────────────────
        if (playing) m64_fpscam_update(&g_cam, in, game_dt);

        // ── Veil ───────────────────────────────────────────────────────
        // Z is the toggle: held up, released down. A press-to-latch toggle
        // makes it too easy to leave up and forget, and the whole design
        // is about managing when it is up.
        pm_veil_set(&veil, playing && (in->buttons & M64_BTN_Z) != 0);
        // An overlord's halo overrides that. Queried before the update so
        // the force lands on the same frame the player walks into it.
        pm_veil_force(&veil, playing ? pm_demons_veil_force_at(g_cam.pos) : 0.0f);
        pm_veil_update(&veil, dt);
        // Writes fog + the rebated far plane into the scene, so it has to
        // come before m64_scene_update rebuilds the projection.
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
            // Events land before the actors' own updates, per m64_event.h.
            m64_event_process(game_dt);
            m64_actor_update_all(game_dt);
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
        if (screen == PM_SCREEN_BOOT) {
            // The splash already wrote the scene's camera in
            // pm_screens_update; anything else here would overwrite it.
        } else if (fps_active) {
            m64_fpscam_apply(&g_cam, &scene);
        } else {
            // The scripted camera owns the scene on every other screen.
            m64_camera_apply(&cam, &scene);
        }
        // After the camera, before the matrices: the shake belongs in the
        // view rather than fighting the damper that produced it.
        pm_fx_apply_camera(&scene);
        m64_scene_update(&scene);

        // ── Draw ───────────────────────────────────────────────────────
        m64_frame_begin();
        m64_scene_begin(&scene);

        if (playing) {
            m64_map_draw(&g_lab);
            // Demon bodies gate themselves on the veil inside their draw
            // callback — that gate is the phantom rule and it is why a
            // corridor can hold a dozen of them.
            m64_actor_draw_all();
        } else {
            pm_screens_draw3d(&app);
        }

        m64_gui_begin();
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
        // And the debug readout over THAT, in the debug ROM only — it is
        // describing what is underneath it, including the fade. Compiles
        // to nothing without M64_DEBUG; see pm_debug.h.
        pm_debug_draw(&app, &scene, fps_active ? &g_cam : NULL, &veil,
                      dt, PM_SCREEN_W, PM_SCREEN_H);
        m64_gui_end();

        m64_frame_end();
    }
}
