// SPDX-License-Identifier: MPL-2.0
//
// pm_screens.c — see pm_screens.h.

#include "pm_screens.h"

#include <libdragon.h>
#include <string.h>

#include <m64/m64_gui.h>
#include <m64/m64_save.h>
#include <m64/m64_audio.h>
#include <m64/m64_splash.h>

#include "pm_demo.h"
#include "pm_models.h"
#include "pm_music.h"
#include "pm_env.h"
#include "pm_lab.h"
#include "pm_arrival.h"
#include "pm_intake.h"
#include "pm_fx.h"
#include "pm_hud.h"

#define BOOT_SECONDS    2.0f
#define ATTRACT_IDLE    12.0f  // OoT's shape: the title gives up after a while
#define FADE_SPEED      2.5f   // 1/seconds; 0.4 s to black and back

// The skull. Authored art rather than generated — see README. CI4 on
// purpose: a 16-entry palette is the veil's own TLUT format, so this can
// later ride pm_veil_bind_palette and bleed red as the filter rises.
#define SKULL_PATH "rom:/sprites/skull.sprite"
static sprite_t *g_skull;
static int       g_skull_tried;

static sprite_t *skull(void)
{
    if (!g_skull && !g_skull_tried) {
        g_skull_tried = 1;
        // sprite_load asserts on a missing file, so the text-only fallback
        // this function promises only exists because of the probe.
        g_skull = m64_dfs_exists(SKULL_PATH) ? sprite_load(SKULL_PATH) : NULL;
        if (!g_skull) debugf("pm_screens: no %s, drawing the text title\n",
                             SKULL_PATH);
    }
    return g_skull;
}

// ── Transitions ────────────────────────────────────────────────────────
static void go(PMApp *app, PMScreen next)
{
    app->screen = next;
    app->screen_t = 0.0f;
}

/** Transition through black, for screens whose entry runs a blocking
 *  pm_models_preload/pm_demo_play. Menu-to-menu moves use plain go(): a
 *  fade there would just make the UI feel slow.
 *
 *  `fn` is deferred, NOT run here — this used to switch `screen` and set
 *  `fade = 1.0` in the same call as the (often already-run) blocking load,
 *  which meant the load's stall showed the PREVIOUS screen frozen on
 *  screen, with black only appearing on the frame after the load had
 *  already finished. go_fade now only arms the transition: it snaps
 *  `fade` to 1.0 and stashes `next`/`fn` on `app`. `fade == 1.0` is drawn
 *  and presented for this screen's current, still-unchanged frame; only on
 *  the NEXT pm_screens_update() call — meaning that solid black frame has
 *  already gone out — does the pending-resolve block at the top of
 *  pm_screens_update run `fn` and perform the actual switch. One mechanism
 *  covers every caller; see PMApp's `pending_fn` for the contract. */
static void go_fade(PMApp *app, PMScreen next, PMTransitionFn fn)
{
    app->pending_screen = next;
    app->pending_fn = fn;
    app->pending = 1;
    app->fade = 1.0f;
}

/** Same guarantee as go_fade, for a blocking load that does not change
 *  which screen is showing — e.g. the attract reel advancing to a shot
 *  whose models were never loaded. A brief black flash covers the stall
 *  instead of a frozen frame. */
static void gate_load(PMApp *app, PMTransitionFn fn)
{
    go_fade(app, app->screen, fn);
}

/** Any button at all, for "press anything to wake the title". */
static int any_button(const M64Input *in)
{
    return in->edges != 0;
}

// ── BOOT ───────────────────────────────────────────────────────────────
static void boot_update(PMApp *app, const M64Input *in, float dt)
{
    // The M64 boot splash owns this screen entirely — its own camera, its
    // own fades, its own jingle. See engine/src/m64/m64_splash.h.
    m64_splash_update(dt, in);
    if (m64_splash_done()) {
        // The splash has already faded itself to black, so the title comes
        // up from black without a second transition on top.
        app->fade = 1.0f;
        go(app, PM_SCREEN_TITLE);
        // The logo model was only ever a handle for m64_splash_init above;
        // nothing draws it again after this frame. Freeing it here, not at
        // some later "who still needs this" audit, is what keeps a model
        // that is provably done with from riding in RAM for the rest of
        // the session — see pm_models_unload's header comment.
        pm_models_unload(PM_MODEL_M64_LOGO);
    }
}

// ── TITLE ──────────────────────────────────────────────────────────────
static void title_update(PMApp *app, const M64Input *in, float dt)
{
    if (any_button(in) || in->stick_x != 0.0f || in->stick_y != 0.0f) {
        app->idle_t = 0.0f;
    } else {
        app->idle_t += dt;
        if (app->idle_t >= ATTRACT_IDLE) {
            app->idle_t = 0.0f;
            app->reel = 0;
            pm_demo_play(&pm_demo_reel[0], 0);
            go(app, PM_SCREEN_ATTRACT);
            return;
        }
    }

    if (in->edges & (M64_BTN_START | M64_BTN_A)) {
        m64_menu_init(&app->menu, PM_SAVE_SLOTS, 0);
        go(app, PM_SCREEN_FILE);
    }
}

// ── ATTRACT ────────────────────────────────────────────────────────────
static void advance_attract_reel(PMApp *app)
{
    pm_demo_play(&pm_demo_reel[app->reel], 0);
}

static void attract_update(PMApp *app, const M64Input *in, float dt)
{
    (void)dt;
    if (any_button(in)) {
        // Straight back to the flyover, looping, so the title always has
        // the same background whichever shot the reel was on. Shot 0 is
        // always already loaded (preloaded at pm_app_init), so this is
        // never a first-load stall and stays a plain, immediate go().
        pm_demo_play(&pm_demo_reel[0], 1);
        app->idle_t = 0.0f;
        go(app, PM_SCREEN_TITLE);
        return;
    }
    if (pm_demo_done()) {
        // The last shot in the reel is the centaur, and it stays locked
        // until someone has seen him where he is meant to be seen.
        const int shots = app->seen_reveal ? pm_demo_reel_count
                                           : pm_demo_reel_count - 1;
        app->reel = (app->reel + 1) % shots;
        // Unlike shot 0, a later reel entry (lab, centaur) may be having
        // its models loaded for the first time this boot — gate it so
        // that stall is a brief black flash, not a frame frozen mid-reel.
        gate_load(app, advance_attract_reel);
    }
}

// ── FILE ───────────────────────────────────────────────────────────────
static void file_refresh(PMApp *app)
{
    for (int i = 0; i < PM_SAVE_SLOTS; i++)
        app->slot_used[i] = app->save_ok ? (uint8_t)m64_save_exists(i) : 0;
}

// A profile that exists resumes. The intro is not replayed — that is the
// whole reason the file screen gates it.
static void enter_play_from_file(PMApp *app)
{
    memset(&app->save, 0, sizeof app->save);
    m64_save_read(app->slot, &app->save);
    app->intro = 0;
    pm_demo_stop();
    // The attract reel's palms are never drawn again in a normal
    // playthrough — see pm_models_unload's header comment.
    pm_models_unload(PM_MODEL_PALMS);
}

static void enter_lab_cine_from_file(PMApp *app)
{
    memset(&app->save, 0, sizeof app->save);
    if (app->save_ok) m64_save_write(app->slot, &app->save);
    file_refresh(app);
    app->intro = 1;
    pm_demo_stop();
    pm_models_unload(PM_MODEL_PALMS);
    pm_models_preload(PM_MODEL_LAB);
    pm_demo_play(&pm_demo_lab_cine, 0);
}

static void file_update(PMApp *app, const M64Input *in, float dt)
{
    (void)dt;

    if (in->edges & M64_BTN_DU) m64_menu_move(&app->menu, -1);
    if (in->edges & M64_BTN_DD) m64_menu_move(&app->menu, +1);
    if (in->edges & M64_BTN_B) {
        pm_demo_play(&pm_demo_reel[0], 1);
        go(app, PM_SCREEN_TITLE);
        return;
    }

    if (in->edges & (M64_BTN_A | M64_BTN_START)) {
        app->slot = app->menu.cursor;
        if (app->slot_used[app->slot])
            go_fade(app, PM_SCREEN_PLAY, enter_play_from_file);
        else
            go_fade(app, PM_SCREEN_LAB_CINE, enter_lab_cine_from_file);
    }
}

// ── The intro screens ──────────────────────────────────────────────────
// Each is "run that beat, then move on". The beats themselves live in
// pm_demo (the scripted shots), pm_lab (the playable section) and
// pm_intake (the transformation) — see pm_screens.h.
//
// Until those land, each one holds for a beat and advances, so the whole
// flow is walkable end to end and the ordering is testable before any of
// the content exists. That is deliberate: a state machine whose states are
// all stubs still tells you whether the state machine is right.
static void skip_intro_to_play(PMApp *app)
{
    (void)app;
    pm_demo_stop();
    pm_models_unload(PM_MODEL_PALMS);
}

static void enter_lab_from_cine(PMApp *app)
{
    // The cinematic's last keyframe is Horner's eye; first person picks up
    // from exactly there so the cut does not jump.
    pm_demo_stop();
    pm_lab_enter(app->fpscam, pm_lab_start_eye(), pm_lab_start_yaw());
}

static void enter_intake_from_lab_cine(PMApp *app)
{
    (void)app;
    pm_demo_play(&pm_intake_shot, 0);
}

static void enter_sub_from_intake(PMApp *app)
{
    (void)app;
    pm_demo_play(&pm_arrival_sub, 0);
}

static void enter_beach_from_sub(PMApp *app)
{
    (void)app;
    pm_demo_play(&pm_arrival_beach, 0);
}

static void finish_intro_to_play(PMApp *app)
{
    app->save.flags |= PM_FLAG_SEEN_REVEAL;
    app->seen_reveal = 1;
    if (app->save_ok && app->slot >= 0)
        m64_save_write(app->slot, &app->save);
    pm_demo_stop();
}

// ── LAB's own two exits (pm_screens_update's own switch, not intro_update:
// LAB is the one screen the player drives rather than a scripted beat) ──
static void leave_lab_to_intake(PMApp *app)
{
    (void)app;
    pm_lab_leave();
    pm_demo_play(&pm_intake_shot, 0);
}

static void leave_lab_to_play(PMApp *app)
{
    (void)app;
    pm_lab_leave();
    pm_demo_stop();
}

static void intro_update(PMApp *app, const M64Input *in, float dt)
{
    (void)dt;

    if (in->edges & M64_BTN_START) {  // skip the whole intro
        go_fade(app, PM_SCREEN_PLAY, skip_intro_to_play);
        return;
    }

    // LAB is the one intro screen the player drives, so it has no hold:
    // it ends when he activates the MRI, which pm_screens_update handles
    // before this runs. Everything else is a scripted beat on a clock.
    // SUB and BEACH end when their shot ends, so their length lives with
    // the shot rather than being duplicated here and drifting from it.
    // LAB_CINE is the same but its shot is pm_demo_lab_cine. INTAKE is
    // still a hold until pm_intake lands.
    // Every scripted beat now ends when its shot ends, so no length is
    // duplicated here to drift from the shot that owns it. LAB is the
    // exception and has no clock at all: the player ends it.
    if (app->screen == PM_SCREEN_LAB) return;
    if (!pm_demo_done()) return;

    switch (app->screen) {
    case PM_SCREEN_LAB_CINE:
        go_fade(app, PM_SCREEN_LAB, enter_lab_from_cine);
        break;
    case PM_SCREEN_LAB:
        go_fade(app, PM_SCREEN_INTAKE, enter_intake_from_lab_cine);
        break;
    case PM_SCREEN_INTAKE:
        // This used to be a plain go() with no fade at all: the intake's
        // last beat is a white flash held over the cut, and the comment
        // here argued the black cover was unneeded on top of it. It WAS
        // needed — sub_setup loads five models (LOACH, ISLAND, SKYDOME,
        // STORM, SEA) and the white flash has already decayed to ~0 alpha
        // by the time that runs, so the stall showed nothing but a frozen
        // last frame of intake. Gating it costs one brief black flash
        // right after the white one; still cheap next to an invisible
        // multi-model stall.
        go_fade(app, PM_SCREEN_SUB, enter_sub_from_intake);
        break;
    case PM_SCREEN_SUB:
        // Same reasoning as INTAKE above: beach_setup preloads LOACH,
        // ISLAND and GUARD, and the old plain go() left that stall
        // uncovered to preserve a same-island visual match-cut. The flash
        // is brief enough not to break that continuity in practice.
        go_fade(app, PM_SCREEN_BEACH, enter_beach_from_sub);
        break;
    default:  // BEACH -> PLAY: the intro is over and the reveal is spent
        go_fade(app, PM_SCREEN_PLAY, finish_intro_to_play);
        break;
    }
}

// ── Public ─────────────────────────────────────────────────────────────
void pm_app_init(PMApp *app, M64FpsCam *fpscam)
{
    memset(app, 0, sizeof *app);
    app->fpscam = fpscam;
    // The engine's default is violet panels / green accent — fine on its
    // own, but this game already has a house palette (pm_hud.h) and having
    // the file-select menu wear a different one is the one place the old
    // three-palette split showed. Start from the default so every
    // zeroed "funk" field (m64_widget.h) stays off, then override just the
    // six colors.
    app->style = m64_widget_style_default();
    app->style.bg     = PM_UI_PANEL;
    app->style.border = PM_UI_BORDER;
    app->style.text   = PM_UI_INK;
    app->style.dim    = PM_UI_DIM;
    app->style.accent = PM_UI_ACCENT;
    app->style.warn   = PM_UI_WARN;
    app->screen = PM_SCREEN_BOOT;
    app->fade = 1.0f;
    app->slot = -1;

    // A console with no EEPROM (or an emulator with the save type unset)
    // still boots and still plays; it just cannot keep a profile. Better
    // than refusing to start, and the file screen says so rather than
    // offering a save that silently evaporates.
    app->save_ok = (m64_save_init(PM_SAVE_SLOTS, sizeof(PMSaveData), 1) == 0);
    if (!app->save_ok) debugf("pm_screens: saves unavailable\n");
    file_refresh(app);

    // The ambience bed. Baked, looping, one mixer channel — it runs under
    // the title, the attract reel and the whole intro, and the only thing
    // that ever changes is its volume. See PetaByte-Madness/dsp/pm_drone.dsp.
    //
    // NOTE THE PATH. mkBakedInstrument writes to the DFS ROOT
    // (filesystem/<name>.wav64), while assetLib.mkSound writes under
    // sfx/. Two builders, two conventions — and getting it wrong is
    // silent, because a failed load here just means the game runs without
    // music. This one cost a while of wondering why it was quiet.
    app->drone = m64_sfx_load("rom:/pmdrone.wav64");
    if (app->drone >= 0) {
        m64_sfx_play(app->drone, PM_CH_DRONE, 255);
        m64_sfx_set_vol_pan(PM_CH_DRONE, 0.0f, 0.5f);
    } else {
        debugf("pm_screens: no drone; running silent\n");
    }

    // The theme, in both of its forms. See pm_music.h — the title plays
    // the score and then the recording of the same quartet.
    pm_music_init();

    // The boot splash: the M64 mark and DeMoD's, before anything of this
    // game's is on screen. Both assets are optional — a missing model
    // draws the fallback wordmark, a missing jingle is silent, and the
    // timing is identical either way.
    m64_splash_init(pm_models_get(PM_MODEL_M64_LOGO),
                    m64_sfx_load("rom:/m64jingle.wav64"),
                    "Made by DeMoD LLC");

    m64_menu_init(&app->menu, PM_SAVE_SLOTS, 0);
    pm_demo_play(&pm_demo_reel[0], 1);
}

PMScreen pm_screens_update(PMApp *app, const M64Input *in, M64Camera *cam,
                           M64Scene *scene, PMVeil *veil, float dt)
{
    (void)veil;

    // A go_fade() call armed last frame: `fade` was snapped to 1.0 and this
    // function returned, so main.c's draw of THAT frame — solid black —
    // has already gone out before this call could ever run. Only now is it
    // safe to run the deferred, possibly-blocking action and switch
    // screens; doing it any earlier is exactly the bug go_fade's own
    // comment describes. Resolved before the fade-decay step below so the
    // fade-in starts on the same frame the new screen actually appears.
    if (app->pending) {
        if (app->pending_fn) app->pending_fn(app);
        go(app, app->pending_screen);
        app->pending = 0;
        app->pending_fn = NULL;
    }

    app->screen_t += dt;
    pm_fx_update(dt);
    // The swell and the foam drift run on wall time, not on whether a
    // particular shot is up, so they stay continuous across a cut.
    pm_env_update(dt);

    // The drone fades up with the picture and ducks for the beach, where
    // the shotgun and the scream need the room. One lerp, no crossfade
    // machinery: there is exactly one bed and it never changes track.
    if (app->drone >= 0) {
        const float want = (app->screen == PM_SCREEN_BOOT)  ? 0.00f
                         : (app->screen == PM_SCREEN_BEACH) ? 0.30f
                         : (app->screen == PM_SCREEN_PLAY)  ? 0.45f
                                                            : 0.70f;
        // Clamped, not just `dt * 1.5f`: the frame right after a gated
        // transition's blocking load can report a dt of several hundred
        // ms (the load stalls the main loop; the NEXT dt measurement
        // covers that whole stall — see go_fade's comment), and an
        // uncapped coefficient above 1 overshoots `want` and rings for a
        // frame or two rather than converging.
        const float k = dt * 1.5f;
        app->drone_vol += (want - app->drone_vol) * (k > 1.0f ? 1.0f : k);
        m64_sfx_set_vol_pan(PM_CH_DRONE, app->drone_vol * (1.0f - app->fade),
                            0.5f);
    }

    // The theme owns the front of the game: the title, the reel that plays
    // when you leave it alone, and the file screen. It ends at LAB_CINE —
    // once the game starts telling its story the drone is the whole bed.
    const int theme_screen = (app->screen == PM_SCREEN_TITLE
                              || app->screen == PM_SCREEN_ATTRACT
                              || app->screen == PM_SCREEN_FILE);
    pm_music_set_active(theme_screen);
    // Under the fade, like the drone — a theme that keeps playing at full
    // level through a fade to black is the seam showing.
    pm_music_update(dt, 0.85f * (1.0f - app->fade));

    // Fade to clear on every screen except BOOT, which fades in from black.
    // BOOT does its own fading inside the splash, so the app-level fade
    // is simply off there.
    const float target = 0.0f;
    if (app->fade > target) {
        app->fade -= FADE_SPEED * dt;
        if (app->fade < target) app->fade = target;
    } else if (app->fade < target) {
        app->fade = target;
    }
    if (app->fade < 0.0f) app->fade = 0.0f;

    switch (app->screen) {
    case PM_SCREEN_BOOT:    boot_update(app, in, dt);    break;
    case PM_SCREEN_TITLE:   title_update(app, in, dt);   break;
    case PM_SCREEN_ATTRACT: attract_update(app, in, dt); break;
    case PM_SCREEN_FILE:    file_update(app, in, dt);    break;
    case PM_SCREEN_PLAY:    break;
    case PM_SCREEN_LAB:
        // The one intro screen the player drives. It ends when he chooses
        // to end it.
        if (pm_lab_update(app->fpscam, in, dt)) {
            go_fade(app, PM_SCREEN_INTAKE, leave_lab_to_intake);
        } else if (in->edges & M64_BTN_START) {
            go_fade(app, PM_SCREEN_PLAY, leave_lab_to_play);
        }
        break;
    default:                intro_update(app, in, dt);   break;
    }

    // Every screen before PLAY is driven by a scripted camera. Pushing
    // CUTSCENE once here (rather than per screen) keeps the mode stack
    // balanced: PLAY is the only screen that pops it.
    // Scripted screens run the shot director. PLAY and LAB are the two
    // that own their own camera (both first person, both m64_fpscam), so
    // they must not have a cutscene pose written over them.
    const int scripted = (app->screen != PM_SCREEN_PLAY
                          && app->screen != PM_SCREEN_LAB
                          && app->screen != PM_SCREEN_BOOT);
    if (scripted) {
        if (cam->mode != M64_CAM_CUTSCENE) m64_camera_push(cam, M64_CAM_CUTSCENE);
        pm_demo_update(dt);
        pm_demo_apply(cam);
        pm_demo_apply_frustum(scene);
        // REQUIRED. m64_camera_set_cutscene only stores cutscene_eye/look;
        // m64_camera_update is what runs the per-mode step that copies them
        // into cam->eye / cam->look, which is what m64_camera_apply reads.
        // Without it both stay at their init values, the view vector is
        // zero-length, normalising it produces a NaN and the VR4300 raises
        // "Floating point invalid operation" — a panic several layers away
        // from the omission. examples/cinematic-demo:809 calls it for
        // exactly this reason and says so; this code copied the shot list
        // from there and dropped the line.
        m64_camera_update(cam, (fm_vec3_t){{ 0, 0, 0 }}, 0.0f, dt);
    } else if (cam->mode == M64_CAM_CUTSCENE) {
        m64_camera_pop(cam);
    }
    if (app->screen == PM_SCREEN_BOOT) m64_splash_apply(scene);

    return app->screen;
}

void pm_screens_draw3d(PMApp *app)
{
    if (app->screen == PM_SCREEN_BOOT) m64_splash_draw3d();
    else if (app->screen == PM_SCREEN_LAB) pm_lab_draw3d();
    else if (app->screen != PM_SCREEN_PLAY) pm_demo_draw();
}

// ── 2D ─────────────────────────────────────────────────────────────────
static void draw_title(PMApp *app, int w, int h)
{
    // One panel behind the skull + title, in the same chrome the HUD and
    // the file/lab screens now share — legible over the live flyover
    // instead of text floating directly on top of it. Sized to the
    // skull + title block only, not the whole screen: the point of the
    // moving background is that it's still visibly the game underneath.
    m64_gui_panel(w / 2 - 100, 14, 200, 126, PM_UI_PANEL, PM_UI_BORDER);

    sprite_t *s = skull();
    if (s) {
        // Centred above the menu. rdpq_sprite_upload + a rectangle is the
        // same path examples/assets-demo uses for its logo.
        const int sw = s->width, sh = s->height;
        rdpq_sprite_upload(TILE0, s, NULL);

        // The sprite is CI4 with a real alpha channel baked in at build
        // time (nix's pmSkull, mksprite -> a 16-entry RGBA5551 palette,
        // one entry alpha=0) — but m64_gui_begin() runs the whole 2D pass
        // with alphacompare(0), i.e. OFF, so every other GUI primitive's
        // flat fills and text glyphs aren't affected by a texture's alpha
        // bit. Without re-enabling it here, the RDP still samples the
        // "transparent" palette entry's RGB (white) and draws it opaque,
        // which is the ugly white box around the art. Push/pop scopes the
        // cutout to just this one draw.
        rdpq_mode_push();
        rdpq_mode_alphacompare(1);
        rdpq_texture_rectangle(TILE0, (w - sw) / 2, 26,
                               (w - sw) / 2 + sw, 26 + sh, 0, 0);
        rdpq_mode_pop();
    } else {
        // No art yet: the title still works. Deliberately not a silent
        // blank — a missing asset should look like a missing asset.
        m64_gui_text(w / 2 - 48, 52, PM_UI_WARN, "[ SKULL ]");
    }

    m64_gui_text(w / 2 - 60, 116, PM_UI_INK, "PETABYTE MADNESS");

    // Blink, so it reads as an invitation rather than a label. Left off
    // the panel above on purpose — it sits well clear of it, near the
    // bottom of the screen, and blinking chrome would compete with the
    // text for the eye instead of framing it.
    if ((int)(app->screen_t * 1.6f) & 1) {
        m64_gui_text(w / 2 - 44, h - 44, PM_UI_DIM, "PRESS START");
    }
}

static void draw_file(PMApp *app, int w, int h)
{
    (void)h;
    m64_gui_text(w / 2 - 40, 22, PM_UI_INK, "SELECT FILE");

    static char rows[PM_SAVE_SLOTS][28];
    const char *labels[PM_SAVE_SLOTS];
    for (int i = 0; i < PM_SAVE_SLOTS; i++) {
        if (app->slot_used[i]) {
            PMSaveData d;
            memset(&d, 0, sizeof d);
            m64_save_read(i, &d);
            snprintf(rows[i], sizeof rows[i], "FILE %d   %02u:%02u",
                     i + 1, (unsigned)(d.playtime / 3600),
                     (unsigned)((d.playtime / 60) % 60));
        } else {
            snprintf(rows[i], sizeof rows[i], "FILE %d   - NEW -", i + 1);
        }
        labels[i] = rows[i];
    }

    m64_menu_draw(&app->menu, w / 2 - 78, 48, 156, labels, NULL, &app->style);

    if (!app->save_ok) {
        m64_gui_text(w / 2 - 76, 128, PM_UI_WARN, "NO EEPROM - NOTHING SAVES");
    }
}

void pm_screens_draw2d(PMApp *app, int w, int h)
{
    switch (app->screen) {
    case PM_SCREEN_BOOT:  m64_splash_draw2d(w, h); break;
    case PM_SCREEN_TITLE: draw_title(app, w, h); break;
    case PM_SCREEN_FILE:  draw_file(app, w, h);  break;
    case PM_SCREEN_LAB:   pm_lab_draw2d(w, h); break;
    case PM_SCREEN_ATTRACT:
        // Nothing. The reel is the screen — a HUD over it would give away
        // that this is a menu background rather than the game.
        break;
    default:
        break;
    }

    // Letterbox, flash and the binary go over the HUD but UNDER the
    // fade — a transition to black should take the bars with it, not
    // leave them floating on an empty screen.
    pm_fx_draw(w, h);

    if (app->fade > 0.001f) {
        m64_gui_rect(0, 0, w, h,
                     RGBA32(0, 0, 0, (uint8_t)(app->fade * 255.0f)));
    }
}
