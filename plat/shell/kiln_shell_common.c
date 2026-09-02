/* SPDX-License-Identifier: MIT
 *
 * kiln_shell_common.c — everything both launchers must agree about.
 *
 * The native and browser backends differ in about twenty lines each: one gets
 * a surface from SDL, the other from a canvas. Everything else — which key is
 * the A button, how far a frame is allowed to slip before the pacer gives up
 * chasing it, what --dfs does — is here, because two launchers that disagreed
 * about the A button would be two games.
 */
#include "kiln_shell.h"

#include <libdragon.h>
#include <kiln_host.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── argv ──────────────────────────────────────────────────────────── */

static const char *USAGE =
"kiln host launcher\n"
"\n"
"  --dfs <dir>       asset directory the ROM's rom:/ paths resolve against\n"
"  --eeprom <file>   save file (default kiln-eeprom.bin)\n"
"  --title <text>    window title\n"
"  --scale <n>       integer window scale (default: fit the display)\n"
"  --fps <n>         target frame rate (default 60)\n"
"  --frames <n>      quit after n frames\n"
"  --shot <file.png> write the last frame and quit (implies --frames 1)\n"
"  --stats           print pixel statistics before quitting\n"
"  --fullscreen\n"
"  --mute\n"
"  --help\n";

int kiln_shell_args(int argc, char **argv, KilnShellOpts *o)
{
    memset(o, 0, sizeof *o);
    o->fps = 60;
    o->title = "Kiln";

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;
        #define WANT(x) do { if (!next) { fprintf(stderr, "%s needs a value\n", a); \
                                          return -1; } (x) = next; i++; } while (0)
        if      (!strcmp(a, "--dfs"))        WANT(o->dfs);
        else if (!strcmp(a, "--eeprom"))     WANT(o->eeprom);
        else if (!strcmp(a, "--title"))      WANT(o->title);
        else if (!strcmp(a, "--scale"))      { const char *v; WANT(v); o->scale = atoi(v); }
        else if (!strcmp(a, "--fps"))        { const char *v; WANT(v); o->fps = atoi(v); }
        else if (!strcmp(a, "--frames"))     { const char *v; WANT(v); o->frames = atoi(v); }
        else if (!strcmp(a, "--shot"))       WANT(o->shot);
        else if (!strcmp(a, "--stats"))      o->stats = 1;
        else if (!strcmp(a, "--fullscreen")) o->fullscreen = 1;
        else if (!strcmp(a, "--mute"))       o->mute = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            fputs(USAGE, stdout);
            fputs("\n", stdout);
            fputs(kiln_shell_keymap_text(), stdout);
            return 1;
        } else {
            fprintf(stderr, "unknown option: %s\n\n%s", a, USAGE);
            return -1;
        }
        #undef WANT
    }
    if (o->fps <= 0) o->fps = 60;
    return 0;
}

#ifndef KILN_SHELL_DEFAULT_DFS
#  define KILN_SHELL_DEFAULT_DFS ""
#endif

static KilnShellOpts g_opts;
static int g_frames;

void kiln_shell_presented(void)
{
    g_frames++;
    const int limit = g_opts.frames ? g_opts.frames : (g_opts.shot ? 1 : 0);
    if (!limit || g_frames < limit) return;

    /* The same colour histogram and non-black percentage tools/n64-shot.sh
     * prints for an emulator capture, so a launcher run and a console capture
     * are read the same way. CLAUDE.md: trust the pixel statistics, not your
     * eyes — a mostly-black 320x240 frame scaled into a screenshot reads as
     * "black" when the text is right there. */
    if (g_opts.stats) kiln_host_stats(stdout, 6);

    if (g_opts.shot) {
        if (kiln_host_capture(g_opts.shot) != 0) {
            fprintf(stderr, "kiln: could not write %s\n", g_opts.shot);
            exit(1);
        }
        fprintf(stderr, "kiln: wrote %s after %d frames\n", g_opts.shot, g_frames);
    }
    exit(0);
}

void kiln_shell_env(const KilnShellOpts *o)
{
    g_opts = *o;

    /* A packaged build knows where its own assets are — nix/host.nix bakes
     * the merged filesystem path in — so `kiln-engine-demo` with no arguments
     * runs the game rather than failing to find rom:/ paths. --dfs still wins,
     * which is what makes iterating on assets outside the store possible. */
    if (!o->dfs && KILN_SHELL_DEFAULT_DFS[0])
        setenv("KILN_HOST_DFS", KILN_SHELL_DEFAULT_DFS, 0);

    /* host_io.c reads both of these once, at dfs_init/eepfs_init time, which
     * is inside the game's own start-up — so they have to be set before
     * kiln_game_main is called, not alongside it. */
    if (o->dfs)    setenv("KILN_HOST_DFS", o->dfs, 1);
    if (o->eeprom) setenv("KILN_HOST_EEPROM", o->eeprom, 1);
}

/* ── the pad ───────────────────────────────────────────────────────── */

void kiln_shell_pad(const KilnShellPad *p)
{
    joypad_inputs_t in;
    memset(&in, 0, sizeof in);
    if (p) {
        in.stick_x     = p->stick_x;
        in.stick_y     = p->stick_y;
        in.btn.a       = p->a;
        in.btn.b       = p->b;
        in.btn.z       = p->z;
        in.btn.l       = p->l;
        in.btn.r       = p->r;
        in.btn.start   = p->start;
        in.btn.c_up    = p->c_up;
        in.btn.c_down  = p->c_down;
        in.btn.c_left  = p->c_left;
        in.btn.c_right = p->c_right;
        in.btn.d_up    = p->d_up;
        in.btn.d_down  = p->d_down;
        in.btn.d_left  = p->d_left;
        in.btn.d_right = p->d_right;
    }
    kiln_host_pad_set(JOYPAD_PORT_1, in);
}

void kiln_shell_pad_from_keys(const unsigned char down[KILN_KEY_COUNT],
                              KilnShellPad *out)
{
    memset(out, 0, sizeof *out);

    /* A keyboard stick is a square and the console's is a disc, so a diagonal
     * held on two keys would read 1.41x full deflection and kiln_input's
     * squared-magnitude deadzone would pass it straight through. 64 is
     * JOYPAD_RANGE_N64_STICK_MAX / sqrt(2), rounded down: full speed on an
     * axis, and a diagonal that lands just inside the rim rather than outside
     * it. */
    const int FULL = JOYPAD_RANGE_N64_STICK_MAX;
    const int DIAG = 64;
    int x = (down[KILN_KEY_RIGHT] ? 1 : 0) - (down[KILN_KEY_LEFT] ? 1 : 0);
    int y = (down[KILN_KEY_UP]    ? 1 : 0) - (down[KILN_KEY_DOWN] ? 1 : 0);
    const int mag = (x && y) ? DIAG : FULL;
    out->stick_x = (int8_t)(x * mag);
    out->stick_y = (int8_t)(y * mag);

    out->d_up    = down[KILN_KEY_DUP];
    out->d_down  = down[KILN_KEY_DDOWN];
    out->d_left  = down[KILN_KEY_DLEFT];
    out->d_right = down[KILN_KEY_DRIGHT];
    out->a       = down[KILN_KEY_A];
    out->b       = down[KILN_KEY_B];
    out->z       = down[KILN_KEY_Z];
    out->l       = down[KILN_KEY_L];
    out->r       = down[KILN_KEY_R];
    out->start   = down[KILN_KEY_START];
    out->c_up    = down[KILN_KEY_CUP];
    out->c_down  = down[KILN_KEY_CDOWN];
    out->c_left  = down[KILN_KEY_CLEFT];
    out->c_right = down[KILN_KEY_CRIGHT];
}

const char *kiln_shell_keymap_text(void)
{
    return
    "keyboard:\n"
    "  W A S D      analog stick        arrows   D-pad\n"
    "  I J K L      C buttons           Enter    Start\n"
    "  Space / .    A                   Comma    B\n"
    "  Left Shift   Z trigger           Q / E    L / R\n"
    "  Esc          quit                F11      fullscreen\n"
    "\n"
    "A connected gamepad is used when present; the keyboard stays live.\n";
}

/* ── pacing ────────────────────────────────────────────────────────── */
/* One deadline, advanced by a fixed step, because every example in this repo
 * integrates a fixed 1/60 dt. Pacing to wall time while simulating a fixed
 * step is what makes a host build run at the speed the console runs at rather
 * than at the speed the rasteriser happens to manage.
 *
 * The catch-up cap matters: without it, a launcher that loses a second to a
 * window resize then runs sixty frames flat out to "make up time", which on a
 * fixed-step simulation is a second of fast-forward. Slipping is the correct
 * response to being late. */
static uint32_t g_next_ms;
static int      g_paced;

void kiln_shell_pace_reset(uint32_t now_ms) { g_next_ms = now_ms; g_paced = 1; }

uint32_t kiln_shell_pace(uint32_t now_ms, int fps)
{
    if (fps <= 0) fps = 60;
    if (!g_paced) kiln_shell_pace_reset(now_ms);

    const uint32_t step = (uint32_t)(1000 / fps);
    g_next_ms += step ? step : 16;

    const int32_t slack = (int32_t)(g_next_ms - now_ms);
    if (slack < -250) {          /* a quarter second behind: stop chasing */
        g_next_ms = now_ms;
        return 0;
    }
    return slack > 0 ? (uint32_t)slack : 0;
}
