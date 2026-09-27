/* SPDX-License-Identifier: MIT
 *
 * kiln_shell.h — what a launcher is allowed to be.
 *
 * plat/host renders a Kiln game and deliberately cannot show it to anyone: it
 * has one framebuffer, no clock, no window and no speaker, because that is
 * what makes nix/checks/ able to compare a PNG byte for byte on four
 * architectures. A launcher supplies exactly the three things a gate must not
 * have — a surface, real time, and a device — through kiln_host.h's
 * FigHostHooks, and nothing else.
 *
 * That boundary is the whole design. A launcher may:
 *   - blit the finished framebuffer,
 *   - pace the frame and pump an event queue,
 *   - push pad state with fig_host_pad_set,
 *   - accept and play audio buffers.
 * It may NOT rasterise, transform, decode a model, or reimplement any part of
 * a kiln_* or libdragon call. The moment it does, the thing on screen stops
 * being evidence about the thing the gates check — which is the mistake
 * nix/checks/kiln-widget.nix caught once already, when tools/uipreview drew
 * its own rectangles and disagreed with the console about panel edge order.
 *
 * ── Two backends, one shell ────────────────────────────────────────────
 * shell_sdl.c is the native window (Linux/BSD/macOS, on every architecture
 * nix/host.nix builds). shell_web.c is the browser, over a canvas and the
 * Gamepad API, and it exists as a second file rather than SDL2-under-
 * Emscripten because -sUSE_SDL=2 downloads its port and a Nix sandbox has no
 * network. They share kiln_shell_common.c: argv, the pad mapping, the frame
 * deadline. Only the twenty lines that touch a surface differ.
 *
 * ── The game's main() ──────────────────────────────────────────────────
 * A game is compiled with -Dmain=fig_game_main, so examples/<x>/main.c stays
 * a ROM's main() — unedited, still `int main(void)`, still a blocking
 * for(;;) — and the launcher owns the real entry point. No example was
 * touched to make this work, and none has a host-only branch in it.
 */
#ifndef FIG_SHELL_H
#define FIG_SHELL_H

#include <stdint.h>

/** The game, renamed by -Dmain=fig_game_main. */
int fig_game_main(void);

/** A pad, before it is packed into libdragon's bitfield. Stick components are
 *  already in the console's -90..90 range: fig_input divides by
 *  JOYPAD_RANGE_N64_STICK_MAX and applies its own radial deadzone, so a
 *  launcher that normalised differently would silently rescale every read. */
typedef struct {
    int8_t stick_x, stick_y;
    unsigned a : 1, b : 1, z : 1, l : 1, r : 1, start : 1;
    unsigned c_up : 1, c_down : 1, c_left : 1, c_right : 1;
    unsigned d_up : 1, d_down : 1, d_left : 1, d_right : 1;
} FigShellPad;

typedef struct {
    const char *dfs;        /* asset directory; becomes $KILN_HOST_DFS   */
    const char *eeprom;     /* save file;      becomes $FIG_HOST_EEPROM */
    const char *title;
    int   scale;            /* integer window scale, 0 = pick one        */
    int   fullscreen;
    int   mute;
    int   fps;              /* target frames per second, default 60      */
    int   frames;           /* stop after N frames, 0 = run forever      */
    const char *shot;       /* write a PNG of the last frame, then exit  */
    /* Write every text run of the last frame as diffable text, then exit.
     * The counterpart to --shot, and the more useful one for a HUD: a
     * regression there is almost always "the wrong string, or the right
     * string in the wrong place", which is exactly what a pixel diff reports
     * worst. fig_host_text_manifest has recorded string, position, colour and
     * measured width all along — this is the flag that reaches it. */
    const char *manifest;
    int   stats;            /* print pixel statistics on the way out     */
} FigShellOpts;

/* ── shared by both backends (kiln_shell_common.c) ─────────────────── */

/** Parse argv. Returns 0 on success, 1 if --help was asked for, -1 on error. */
int  fig_shell_args(int argc, char **argv, FigShellOpts *o);

/** Apply the options that are environment variables to the host backend, and
 *  remember the options for fig_shell_presented. */
void fig_shell_env(const FigShellOpts *o);

/** Call from a backend's present hook, after the blit. Counts only.
 *
 *  It used to also honour --frames and --shot, and that made the launcher
 *  gate hang rather than fail when the present hook was broken: the only
 *  thing terminating the run lived inside the thing under test. Now the run
 *  ends from fig_shell_tick, which is on the vsync path and always runs, and
 *  this counter is what the gate compares against the frames it asked for —
 *  the native equivalent of the browser gate counting putImageData calls in
 *  its DOM stub rather than believing the program's own report. */
void fig_shell_presented(void);

/** Call from a backend's vsync hook, before pacing. Honours --frames and
 *  --shot, which is what makes a launcher testable: a windowed build that
 *  renders N frames and writes a PNG can be checked without a display, so
 *  "it builds" and "it draws the right thing" stop being separate questions.
 *  Does not return if the run is over. */
void fig_shell_tick(void);

/** Pack and hand a pad to the host backend. NULL clears it. */
void fig_shell_pad(const FigShellPad *p);

/** Frame pacing, in one place so both backends wait the same way: returns
 *  the milliseconds to wait before the next frame is due, given a monotonic
 *  `now_ms`, or 0 if it is already late. Call once per frame. */
uint32_t fig_shell_pace(uint32_t now_ms, int fps);

/** Reset the pacer to `now_ms`. Call once, immediately before the first
 *  frame, so a slow start-up is not repaid as a burst of catch-up frames. */
void fig_shell_pace_reset(uint32_t now_ms);

/** The keyboard map, shared so the two backends cannot disagree about it.
 *  `down` is indexed by FigShellKey. */
typedef enum {
    FIG_KEY_UP, FIG_KEY_DOWN, FIG_KEY_LEFT, FIG_KEY_RIGHT,   /* stick */
    FIG_KEY_DUP, FIG_KEY_DDOWN, FIG_KEY_DLEFT, FIG_KEY_DRIGHT,
    FIG_KEY_A, FIG_KEY_B, FIG_KEY_Z, FIG_KEY_L, FIG_KEY_R,
    FIG_KEY_START,
    FIG_KEY_CUP, FIG_KEY_CDOWN, FIG_KEY_CLEFT, FIG_KEY_CRIGHT,
    FIG_KEY_COUNT
} FigShellKey;

/** Fold a key-down table into a pad. */
void fig_shell_pad_from_keys(const unsigned char down[FIG_KEY_COUNT],
                              FigShellPad *out);

/** The one-screen key map, for --help and for the on-screen hint. */
const char *fig_shell_keymap_text(void);

#endif /* FIG_SHELL_H */
