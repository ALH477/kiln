// SPDX-License-Identifier: MPL-2.0
//
// pm_screens.c — see pm_screens.h.

#include "pm_screens.h"

#include <libdragon.h>
#include <string.h>

#include <kiln/kiln_gui.h>
#include <kiln/kiln_save.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_splash.h>

#include "pm_demo.h"
#include "pm_models.h"
#include "pm_music.h"
#include "pm_env.h"
#include "pm_lab.h"
#include "pm_arrival.h"
#include "pm_intake.h"
#include "pm_narration.h"
#include "pm_credits.h"
#include "pm_cine.h"
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
        g_skull = kiln_dfs_exists(SKULL_PATH) ? sprite_load(SKULL_PATH) : NULL;
        if (!g_skull) debugf("pm_screens: no %s, drawing the text title\n",
                             SKULL_PATH);
    }
    return g_skull;
}

// Generated from PM_SCREEN_LIST, so this can never disagree with PMScreen.
// Kept in the shipping ROM rather than behind KILN_DEBUG: it is a dozen
// string literals, and debugf() messages that name a screen are worth more
// than the bytes.
#define PM_SCREEN_NAME(id, name) [id] = name,
static const char *const SCREEN_NAMES[PM_SCREEN_COUNT] = {
    PM_SCREEN_LIST(PM_SCREEN_NAME)
};
#undef PM_SCREEN_NAME

const char *pm_screen_name(PMScreen s)
{
    // "?" rather than NULL — callers hand this to kiln_gui_text as a format
    // string, and a NULL there is the exact boot crash PM_SCREEN_LIST and
    // PM_MODEL_LIST exist to make unrepresentable.
    if (s < 0 || s >= PM_SCREEN_COUNT) return "?";
    return SCREEN_NAMES[s];
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
static int any_button(const KilnInput *in)
{
    return in->edges != 0;
}

#ifdef KILN_DEBUG
// ── The debug jump menu ────────────────────────────────────────────────
//
// Replaces a pair of hand-patched blocks that used to live further down this
// file under a `TEMP TRAILER-CAPTURE HOOK — DO NOT COMMIT` banner: a table of
// forced transitions with hardcoded timings, edited whenever a different
// screen needed capturing, plus an ungated yellow calibration label that would
// have shipped. They existed because reaching INTAKE the normal way takes
// about ninety seconds of boot splash, title, attract idle, file select,
// narration crawl and lab walk — which is a bad loop to iterate a camera in,
// and a worse one to iterate it in twice.
//
// This does the same job as a real feature. It is DEBUG-ROM ONLY, it does not
// touch a save slot, and it is skippable so the debug ROM still boots the game
// normally.
//
// ── It deliberately does NOT reuse the story transitions ────────────────
// The enter_*_from_* functions above are story steps, and several carry
// side effects a jump must not have. enter_narration_from_file ZEROES AND
// WRITES save slot 0 — which the deleted hook did on every boot, because it
// bypassed the file screen and inherited the default `slot == 0`. So each jump
// below does the minimum to make its screen renderable and nothing else.
static int      g_menu_open;
static KilnMenu  g_menu;

// file_refresh is defined further down with the FILE screen it belongs to;
// declared here rather than moved, so the debug facility stays in one block
// instead of reordering the shipping code around it.
static void file_refresh(PMApp *app);

// Screens worth jumping to. BOOT is excluded (it is where the menu lives) and
// so is ATTRACT (it is TITLE's idle state, reachable by waiting one second
// from TITLE, and its own entry needs the reel index TITLE sets).
static const PMScreen JUMPS[] = {
    PM_SCREEN_TITLE, PM_SCREEN_FILE,  PM_SCREEN_NARRATION,
    PM_SCREEN_LAB_CINE, PM_SCREEN_LAB, PM_SCREEN_INTAKE,
    PM_SCREEN_CREDITS,  PM_SCREEN_SUB, PM_SCREEN_BEACH, PM_SCREEN_PLAY,
};
#define JUMP_COUNT ((int)(sizeof JUMPS / sizeof JUMPS[0]))

static void debug_jump(PMApp *app, PMScreen s)
{
    // A jump can come from anywhere, so tear down unconditionally first —
    // whatever shot or room the previous screen installed is not this
    // screen's to inherit.
    pm_demo_stop();
    kiln_sfx_stop(PM_CH_STORY);

    // No slot is selected and none is written. -1 rather than 0 so anything
    // that does try to persist fails visibly instead of silently overwriting
    // the player's first profile, which is the trap the deleted hook fell in.
    app->slot = -1;
    app->intro = 1;

    switch (s) {
    case PM_SCREEN_TITLE:     pm_demo_play(&pm_demo_reel[0], 1); break;
    case PM_SCREEN_FILE:      file_refresh(app);
                              kiln_menu_init(&app->menu, PM_SAVE_SLOTS, 0); break;
    case PM_SCREEN_NARRATION: pm_demo_play(&pm_narration_shot, 0); break;
    case PM_SCREEN_LAB_CINE:  pm_models_preload(PM_MODEL_LAB);
                              pm_demo_play(&pm_demo_lab_cine, 0); break;
    case PM_SCREEN_LAB:       pm_models_preload(PM_MODEL_LAB);
                              pm_lab_enter(app->fpscam, pm_lab_start_eye(),
                                           pm_lab_start_yaw()); break;
    case PM_SCREEN_INTAKE:    pm_demo_play(&pm_intake_shot, 0); break;
    case PM_SCREEN_CREDITS:   pm_demo_play(&pm_credits_shot, 0); break;
    case PM_SCREEN_SUB:       pm_demo_play(&pm_arrival_sub, 0); break;
    case PM_SCREEN_BEACH:     pm_demo_play(&pm_arrival_beach, 0); break;
    case PM_SCREEN_PLAY:      app->intro = 0; break;
    default: break;
    }

    g_menu_open = 0;
    app->fade = 1.0f;   // come up from black, as every real transition does
    go(app, s);
}

// ── The menu must never be able to strand the ROM ──────────────────────
// It blocks BOOT while it is open, so on any setup where the controller is not
// working the debug ROM would sit on it forever — and "forever" includes
// `./dev drive` on a machine where the uinput -> SDL -> ares binding chain
// fails, which is a real and recurring failure mode (tools/n64-drive.sh's own
// header is largely about it, and it is why this menu is not the only way to
// reach a screen — see PM_JUMP_SCREEN below).
//
// So: no input for this long and the menu gives up and boots the game. Chosen
// longer than a human takes to read ten entries and shorter than anyone's
// patience with a ROM that appears hung.
#define DEBUG_MENU_IDLE 12.0f
static float g_menu_idle;

static void debug_menu_update(PMApp *app, const KilnInput *in, float dt)
{
    if (in->edges || in->buttons) g_menu_idle = 0.0f;
    else                          g_menu_idle += dt;

    if (g_menu_idle >= DEBUG_MENU_IDLE) {
        debugf("pm_screens: debug jump menu idle %.0fs, booting normally\n",
               (double)DEBUG_MENU_IDLE);
        g_menu_open = 0;
        return;
    }

    if (in->edges & KILN_BTN_DU) kiln_menu_move(&g_menu, -1);
    if (in->edges & KILN_BTN_DD) kiln_menu_move(&g_menu, +1);
    if (in->edges & (KILN_BTN_A | KILN_BTN_Z)) {
        debug_jump(app, JUMPS[g_menu.cursor]);
        return;
    }
    // START boots the game normally, so the debug ROM is still the game.
    if (in->edges & (KILN_BTN_START | KILN_BTN_B)) g_menu_open = 0;
}

static void debug_menu_draw(int w, int h)
{
    const int bw = 168, bh = 20 + JUMP_COUNT * 10;
    const int bx = (w - bw) / 2, by = (h - bh) / 2;
    kiln_gui_panel(bx, by, bw, bh, RGBA32(0, 0, 0, 220),
                  RGBA32(120, 200, 255, 230));
    kiln_gui_text(bx + 6, by + 12, RGBA32(120, 200, 255, 255),
                 "DEBUG JUMP   start=play");
    for (int i = 0; i < JUMP_COUNT; i++) {
        const int sel = (i == g_menu.cursor);
        kiln_gui_text(bx + 10, by + 24 + i * 10,
                     sel ? RGBA32(255, 240, 120, 255) : RGBA32(200, 200, 200, 255),
                     "%s%s", sel ? "> " : "  ", pm_screen_name(JUMPS[i]));
    }
}
#endif // KILN_DEBUG

// ── BOOT ───────────────────────────────────────────────────────────────
static void boot_update(PMApp *app, const KilnInput *in, float dt)
{
#ifdef KILN_DEBUG
#ifdef PM_JUMP_SCREEN
    // ── Build-time jump: no controller involved at all ──────────────────
    // The menu needs a working pad, and there are two situations where there
    // is not one: a headless-ish capture run whose input bindings did not take
    // (see debug_menu_update's idle timeout), and `./dev shot`, which takes a
    // single screenshot and has no input path by design.
    //
    // `nix build .#pm-jump-intake` builds a ROM that boots STRAIGHT into
    // INTAKE, so `./dev shot pm-jump-intake out.png 4` captures a cutscene
    // four seconds in with nothing to drive. That is the reliable automation
    // path; the menu is the interactive one. Both go through debug_jump(), so
    // neither can drift from the other on what "entering a screen" means.
    // A one-shot rather than a test on screen_t: pm_screens_update advances
    // screen_t before dispatching, so it is never 0 here, and a jump that
    // re-armed itself every frame would restart its shot forever.
    static int jumped;
    if (!jumped) {
        jumped = 1;
        debugf("pm_screens: PM_JUMP_SCREEN -> %s\n",
               pm_screen_name(PM_JUMP_SCREEN));
        debug_jump(app, PM_JUMP_SCREEN);
        return;
    }
#endif
    // The menu takes over BOOT rather than becoming a screen of its own: a
    // new PMScreen would change the shipping enum, PM_SCREEN_COUNT and the
    // save format's `progress` numbering for a facility the shipping ROM does
    // not have. BOOT is also the only screen with nothing to lose — the
    // splash is 3.8 s of logo.
    if (g_menu_open) { debug_menu_update(app, in, dt); return; }
#endif

    // The Kiln boot splash owns this screen entirely — its own camera, its
    // own fades, its own jingle. See engine/src/kiln/kiln_splash.h.
    kiln_splash_update(dt, in);
    if (kiln_splash_done()) {
        // The splash has already faded itself to black, so the title comes
        // up from black without a second transition on top.
        app->fade = 1.0f;
        go(app, PM_SCREEN_TITLE);
        // The logo model was only ever a handle for kiln_splash_init above;
        // nothing draws it again after this frame. Freeing it here, not at
        // some later "who still needs this" audit, is what keeps a model
        // that is provably done with from riding in RAM for the rest of
        // the session — see pm_models_unload's header comment.
        pm_models_unload(PM_MODEL_KILN_LOGO);
    }
}

// ── TITLE ──────────────────────────────────────────────────────────────
static void title_update(PMApp *app, const KilnInput *in, float dt)
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

    if (in->edges & (KILN_BTN_START | KILN_BTN_A)) {
        kiln_menu_init(&app->menu, PM_SAVE_SLOTS, 0);
        go(app, PM_SCREEN_FILE);
    }
}

// ── ATTRACT ────────────────────────────────────────────────────────────
static void advance_attract_reel(PMApp *app)
{
    pm_demo_play(&pm_demo_reel[app->reel], 0);
}

static void attract_update(PMApp *app, const KilnInput *in, float dt)
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
        app->slot_used[i] = app->save_ok ? (uint8_t)kiln_save_exists(i) : 0;
}

// A profile that exists resumes. The intro is not replayed — that is the
// whole reason the file screen gates it.
static void enter_play_from_file(PMApp *app)
{
    memset(&app->save, 0, sizeof app->save);
    kiln_save_read(app->slot, &app->save);
    app->intro = 0;
    pm_demo_stop();
    // The attract reel's palms are never drawn again in a normal
    // playthrough — see pm_models_unload's header comment.
    pm_models_unload(PM_MODEL_PALMS);
}

static void enter_narration_from_file(PMApp *app)
{
    memset(&app->save, 0, sizeof app->save);
    if (app->save_ok) kiln_save_write(app->slot, &app->save);
    file_refresh(app);
    app->intro = 1;
    pm_demo_stop();
    pm_models_unload(PM_MODEL_PALMS);
    pm_demo_play(&pm_narration_shot, 0);
}

// What enter_lab_cine_from_file used to do directly, now reached once the
// crawl finishes rather than the instant the slot is picked — the lab
// model preload moves here with it, since there is no reason to hold it in
// RAM for the several minutes the crawl runs.
static void enter_lab_cine_from_narration(PMApp *app)
{
    (void)app;
    pm_models_preload(PM_MODEL_LAB);
    pm_demo_play(&pm_demo_lab_cine, 0);
}

static void file_update(PMApp *app, const KilnInput *in, float dt)
{
    (void)dt;

    if (in->edges & KILN_BTN_DU) kiln_menu_move(&app->menu, -1);
    if (in->edges & KILN_BTN_DD) kiln_menu_move(&app->menu, +1);
    if (in->edges & KILN_BTN_B) {
        pm_demo_play(&pm_demo_reel[0], 1);
        go(app, PM_SCREEN_TITLE);
        return;
    }

    if (in->edges & (KILN_BTN_A | KILN_BTN_START)) {
        app->slot = app->menu.cursor;
        if (app->slot_used[app->slot])
            go_fade(app, PM_SCREEN_PLAY, enter_play_from_file);
        else
            go_fade(app, PM_SCREEN_NARRATION, enter_narration_from_file);
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
    // PM_CH_STORY (narration/surgery) is not any single shot's teardown to
    // own — pm_demo_stop() above only reaches whichever PMDemoShot is
    // CURRENTLY playing, and the surgery cue outlives LAB_CINE, LAB and
    // INTAKE as shots come and go. Unconditional and harmless if already
    // silent: skipping from LAB or INTAKE is exactly the case that would
    // otherwise leave it playing on into PLAY forever.
    kiln_sfx_stop(PM_CH_STORY);
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

static void enter_credits_from_intake(PMApp *app)
{
    (void)app;
    // The surgery OST (pm_lab.c) has been playing since the player entered
    // the lab, straight through INTAKE's climb-in — it ends HERE, not in
    // pm_intake.c's own teardown, because its owning lifetime is the whole
    // LAB + INTAKE visit rather than just the transformation cutscene.
    kiln_sfx_stop(PM_CH_STORY);
    pm_demo_play(&pm_credits_shot, 0);
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
        kiln_save_write(app->slot, &app->save);
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
    // Same reason as skip_intro_to_play's: the surgery cue started at
    // pm_lab_enter, and nothing else stops it on this path.
    kiln_sfx_stop(PM_CH_STORY);
}

static void intro_update(PMApp *app, const KilnInput *in, float dt)
{
    (void)dt;

    if (in->edges & KILN_BTN_START) {  // skip the whole intro
        go_fade(app, PM_SCREEN_PLAY, skip_intro_to_play);
        return;
    }

    // LAB is the one intro screen the player drives, so it has no hold:
    // it ends when he activates the MRI, which pm_screens_update handles
    // before this runs. Every other screen here — NARRATION, LAB_CINE,
    // INTAKE, CREDITS, SUB, BEACH — is a scripted PMDemoShot on a clock, and
    // ends when ITS shot ends, so no length is duplicated here to drift
    // from the shot that actually owns it.
    if (app->screen == PM_SCREEN_LAB) return;
    if (!pm_demo_done()) return;

    switch (app->screen) {
    case PM_SCREEN_NARRATION:
        go_fade(app, PM_SCREEN_LAB_CINE, enter_lab_cine_from_narration);
        break;
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
        // needed — credits_setup opens the FMV decoder, and the white
        // flash has already decayed to ~0 alpha by the time that runs, so
        // the stall showed nothing but a frozen last frame of intake.
        // Gating it costs one brief black flash right after the white one;
        // still cheap next to an invisible decoder-open stall.
        go_fade(app, PM_SCREEN_CREDITS, enter_credits_from_intake);
        break;
    case PM_SCREEN_CREDITS:
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
void pm_app_init(PMApp *app, KilnFpsCam *fpscam)
{
    memset(app, 0, sizeof *app);
    app->fpscam = fpscam;
    // The engine's default is violet panels / green accent — fine on its
    // own, but this game already has a house palette (pm_hud.h) and having
    // the file-select menu wear a different one is the one place the old
    // three-palette split showed. Start from the default so every
    // zeroed "funk" field (kiln_widget.h) stays off, then override just the
    // six colors.
    app->style = kiln_widget_style_default();
    app->style.bg     = PM_UI_PANEL;
    app->style.border = PM_UI_BORDER;
    app->style.text   = PM_UI_INK;
    app->style.dim    = PM_UI_DIM;
    app->style.accent = PM_UI_ACCENT;
    app->style.warn   = PM_UI_WARN;
    app->screen = PM_SCREEN_BOOT;
    app->fade = 1.0f;
    app->slot = -1;

#ifdef KILN_DEBUG
    // The jump menu comes up first in the debug ROM, over the splash. Opening
    // by default rather than behind a held button is the right way round for
    // `./dev drive`: a capture script can walk the menu with `press down` and
    // `press a`, but it cannot hold a button before the ROM has booted. START
    // dismisses it and the game runs normally from there.
    g_menu_open = 1;
    kiln_menu_init(&g_menu, JUMP_COUNT, 0);
#endif

    // A console with no EEPROM (or an emulator with the save type unset)
    // still boots and still plays; it just cannot keep a profile. Better
    // than refusing to start, and the file screen says so rather than
    // offering a save that silently evaporates.
    app->save_ok = (kiln_save_init(PM_SAVE_SLOTS, sizeof(PMSaveData), 1) == 0);
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
    app->drone = kiln_sfx_load("rom:/pmdrone.wav64");
    if (app->drone >= 0) {
        kiln_sfx_play(app->drone, PM_CH_DRONE, 255);
        kiln_sfx_set_vol_pan(PM_CH_DRONE, 0.0f, 0.5f);
    } else {
        debugf("pm_screens: no drone; running silent\n");
    }

    // The theme, in both of its forms. See pm_music.h — the title plays
    // the score and then the recording of the same quartet.
    pm_music_init();

    // The boot splash: the Kiln mark and DeMoD's, before anything of this
    // game's is on screen. Both assets are optional — a missing model
    // draws the fallback wordmark, a missing jingle is silent, and the
    // timing is identical either way.
    kiln_splash_init(pm_models_get(PM_MODEL_KILN_LOGO),
                    kiln_sfx_load("rom:/kilnjingle.wav64"),
                    "Made by DeMoD LLC");

    kiln_menu_init(&app->menu, PM_SAVE_SLOTS, 0);
    pm_demo_play(&pm_demo_reel[0], 1);
}

PMScreen pm_screens_update(PMApp *app, const KilnInput *in, KilnCamera *cam,
                           KilnScene *scene, PMVeil *veil, float dt)
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
    // Through the transport, so a paused shot is paused WHOLE. pm_fx.h is
    // explicit that hit-stop runs the effects on real time so the shake sells
    // the hit — right in play, and exactly backwards when the thing being
    // studied is one frame of a flash. pm_cine_dt returns `dt` untouched in a
    // shipping build.
    pm_fx_update(pm_cine_dt(dt));
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
        kiln_sfx_set_vol_pan(PM_CH_DRONE, app->drone_vol * (1.0f - app->fade),
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
        } else if (in->edges & KILN_BTN_START) {
            go_fade(app, PM_SCREEN_PLAY, leave_lab_to_play);
        }
        break;
    default:                intro_update(app, in, dt);   break;
    }

    // Every screen before PLAY is driven by a scripted camera. Pushing
    // CUTSCENE once here (rather than per screen) keeps the mode stack
    // balanced: PLAY is the only screen that pops it.
    // Scripted screens run the shot director. PLAY and LAB are the two
    // that own their own camera (both first person, both kiln_fpscam), so
    // they must not have a cutscene pose written over them.
    const int scripted = (app->screen != PM_SCREEN_PLAY
                          && app->screen != PM_SCREEN_LAB
                          && app->screen != PM_SCREEN_BOOT);
    if (scripted) {
        if (cam->mode != KILN_CAM_CUTSCENE) kiln_camera_push(cam, KILN_CAM_CUTSCENE);
        pm_demo_update(pm_cine_dt(dt));
        pm_demo_apply(cam);
        pm_demo_apply_frustum(scene);
        // REQUIRED. kiln_camera_set_cutscene only stores cutscene_eye/look;
        // kiln_camera_update is what runs the per-mode step that copies them
        // into cam->eye / cam->look, which is what kiln_camera_apply reads.
        // Without it both stay at their init values, the view vector is
        // zero-length, normalising it produces a NaN and the VR4300 raises
        // "Floating point invalid operation" — a panic several layers away
        // from the omission. examples/cinematic-demo:809 calls it for
        // exactly this reason and says so; this code copied the shot list
        // from there and dropped the line.
        kiln_camera_update(cam, (fm_vec3_t){{ 0, 0, 0 }}, 0.0f, dt);
    } else if (cam->mode == KILN_CAM_CUTSCENE) {
        kiln_camera_pop(cam);
    }
    if (app->screen == PM_SCREEN_BOOT) kiln_splash_apply(scene);

    return app->screen;
}

void pm_screens_draw3d(PMApp *app)
{
    if (app->screen == PM_SCREEN_BOOT) kiln_splash_draw3d();
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
    kiln_gui_panel(w / 2 - 100, 14, 200, 126, PM_UI_PANEL, PM_UI_BORDER);

    sprite_t *s = skull();
    if (s) {
        // Centred above the menu. rdpq_sprite_upload + a rectangle is the
        // same path examples/assets-demo uses for its logo.
        const int sw = s->width, sh = s->height;
        rdpq_sprite_upload(TILE0, s, NULL);

        // The sprite is CI4 with a real alpha channel baked in at build
        // time (nix's pmSkull, mksprite -> a 16-entry RGBA5551 palette,
        // one entry alpha=0) — but kiln_gui_begin() runs the whole 2D pass
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
        kiln_gui_text(w / 2 - 48, 52, PM_UI_WARN, "[ SKULL ]");
    }

    kiln_gui_text(w / 2 - 60, 116, PM_UI_INK, "PETABYTE MADNESS");

    // Blink, so it reads as an invitation rather than a label. Left off
    // the panel above on purpose — it sits well clear of it, near the
    // bottom of the screen, and blinking chrome would compete with the
    // text for the eye instead of framing it.
    if ((int)(app->screen_t * 1.6f) & 1) {
        kiln_gui_text(w / 2 - 44, h - 44, PM_UI_DIM, "PRESS START");
    }
}

static void draw_file(PMApp *app, int w, int h)
{
    (void)h;
    kiln_gui_text(w / 2 - 40, 22, PM_UI_INK, "SELECT FILE");

    static char rows[PM_SAVE_SLOTS][28];
    const char *labels[PM_SAVE_SLOTS];
    for (int i = 0; i < PM_SAVE_SLOTS; i++) {
        if (app->slot_used[i]) {
            PMSaveData d;
            memset(&d, 0, sizeof d);
            kiln_save_read(i, &d);
            snprintf(rows[i], sizeof rows[i], "FILE %d   %02u:%02u",
                     i + 1, (unsigned)(d.playtime / 3600),
                     (unsigned)((d.playtime / 60) % 60));
        } else {
            snprintf(rows[i], sizeof rows[i], "FILE %d   - NEW -", i + 1);
        }
        labels[i] = rows[i];
    }

    kiln_menu_draw(&app->menu, w / 2 - 78, 48, 156, labels, NULL, &app->style);

    if (!app->save_ok) {
        kiln_gui_text(w / 2 - 76, 128, PM_UI_WARN, "NO EEPROM - NOTHING SAVES");
    }
}

void pm_screens_draw2d(PMApp *app, int w, int h)
{
#ifdef KILN_DEBUG
    // Before the switch and before the fade, so the menu is readable while
    // `fade` is still 1.0 from pm_app_init — the alternative is a menu you
    // cannot see for the first fifth of a second, which a drive script's first
    // screenshot would catch and a human would not.
    if (app->screen == PM_SCREEN_BOOT && g_menu_open) {
        debug_menu_draw(w, h);
        return;
    }
#endif

    switch (app->screen) {
    case PM_SCREEN_BOOT:      kiln_splash_draw2d(w, h); break;
    case PM_SCREEN_TITLE:     draw_title(app, w, h); break;
    case PM_SCREEN_FILE:      draw_file(app, w, h);  break;
    case PM_SCREEN_LAB:       pm_lab_draw2d(w, h); break;
    case PM_SCREEN_NARRATION: pm_narration_draw2d(w, h); break;
    case PM_SCREEN_CREDITS:   pm_credits_draw2d(w, h); break;
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
        kiln_gui_rect(0, 0, w, h,
                     RGBA32(0, 0, 0, (uint8_t)(app->fade * 255.0f)));
    }
}
