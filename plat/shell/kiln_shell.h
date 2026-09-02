/* SPDX-License-Identifier: MIT
 *
 * kiln_shell.h — what a launcher is allowed to be.
 *
 * plat/host renders a Kiln game and deliberately cannot show it to anyone: it
 * has one framebuffer, no clock, no window and no speaker, because that is
 * what makes nix/checks/ able to compare a PNG byte for byte on four
 * architectures. A launcher supplies exactly the three things a gate must not
 * have — a surface, real time, and a device — through kiln_host.h's
 * KilnHostHooks, and nothing else.
 *
 * That boundary is the whole design. A launcher may:
 *   - blit the finished framebuffer,
 *   - pace the frame and pump an event queue,
 *   - push pad state with kiln_host_pad_set,
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
 * A game is compiled with -Dmain=kiln_game_main, so examples/<x>/main.c stays
 * a ROM's main() — unedited, still `int main(void)`, still a blocking
 * for(;;) — and the launcher owns the real entry point. No example was
 * touched to make this work, and none has a host-only branch in it.
 */
#ifndef KILN_SHELL_H
#define KILN_SHELL_H

#include <stdint.h>

/** The game, renamed by -Dmain=kiln_game_main. */
int kiln_game_main(void);

/** A pad, before it is packed into libdragon's bitfield. Stick components are
 *  already in the console's -90..90 range: kiln_input divides by
 *  JOYPAD_RANGE_N64_STICK_MAX and applies its own radial deadzone, so a
 *  launcher that normalised differently would silently rescale every read. */
typedef struct {
    int8_t stick_x, stick_y;
    unsigned a : 1, b : 1, z : 1, l : 1, r : 1, start : 1;
    unsigned c_up : 1, c_down : 1, c_left : 1, c_right : 1;
    unsigned d_up : 1, d_down : 1, d_left : 1, d_right : 1;
} KilnShellPad;

typedef struct {
    const char *dfs;        /* asset directory; becomes $KILN_HOST_DFS   */
    const char *eeprom;     /* save file;      becomes $KILN_HOST_EEPROM */
    const char *title;
    int   scale;            /* integer window scale, 0 = pick one        */
    int   fullscreen;
    int   mute;
    int   fps;              /* target frames per second, default 60      */
    int   frames;           /* stop after N frames, 0 = run forever      */
    const char *shot;       /* write a PNG of the last frame, then exit  */
    int   stats;            /* print pixel statistics on the way out     */
} KilnShellOpts;

/* ── shared by both backends (kiln_shell_common.c) ─────────────────── */

/** Parse argv. Returns 0 on success, 1 if --help was asked for, -1 on error. */
int  kiln_shell_args(int argc, char **argv, KilnShellOpts *o);

/** Apply the options that are environment variables to the host backend, and
 *  remember the options for kiln_shell_presented. */
void kiln_shell_env(const KilnShellOpts *o);

/** Call from a backend's present hook, after the blit. Honours --frames and
 *  --shot, which is what makes a launcher testable: a windowed build that
 *  renders N frames and writes a PNG can be held to the same reference image
 *  the headless gates use, so "it builds" and "it draws the right thing" stop
 *  being separate questions. Does not return if the run is over. */
void kiln_shell_presented(void);

/** Pack and hand a pad to the host backend. NULL clears it. */
void kiln_shell_pad(const KilnShellPad *p);

/** Frame pacing, in one place so both backends wait the same way: returns
 *  the milliseconds to wait before the next frame is due, given a monotonic
 *  `now_ms`, or 0 if it is already late. Call once per frame. */
uint32_t kiln_shell_pace(uint32_t now_ms, int fps);

/** Reset the pacer to `now_ms`. Call once, immediately before the first
 *  frame, so a slow start-up is not repaid as a burst of catch-up frames. */
void kiln_shell_pace_reset(uint32_t now_ms);

/** The keyboard map, shared so the two backends cannot disagree about it.
 *  `down` is indexed by KilnShellKey. */
typedef enum {
    KILN_KEY_UP, KILN_KEY_DOWN, KILN_KEY_LEFT, KILN_KEY_RIGHT,   /* stick */
    KILN_KEY_DUP, KILN_KEY_DDOWN, KILN_KEY_DLEFT, KILN_KEY_DRIGHT,
    KILN_KEY_A, KILN_KEY_B, KILN_KEY_Z, KILN_KEY_L, KILN_KEY_R,
    KILN_KEY_START,
    KILN_KEY_CUP, KILN_KEY_CDOWN, KILN_KEY_CLEFT, KILN_KEY_CRIGHT,
    KILN_KEY_COUNT
} KilnShellKey;

/** Fold a key-down table into a pad. */
void kiln_shell_pad_from_keys(const unsigned char down[KILN_KEY_COUNT],
                              KilnShellPad *out);

/** The one-screen key map, for --help and for the on-screen hint. */
const char *kiln_shell_keymap_text(void);

#endif /* KILN_SHELL_H */
