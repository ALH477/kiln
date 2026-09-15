/* SPDX-License-Identifier: MIT
 *
 * kiln_console.h — retro on-screen debug console.
 *
 * A joypad-typed command line overlaid on the running ROM, intended for the
 * retro dev cycle: boot a cart, see something wrong, interrogate state
 * without a USB tether. Toggle with a four-button chord (hold Start, then
 * C-Up → C-Left → C-Down → C-Right — counter-clockwise around the C cluster)
 * so opening the console never clashes with single-button game inputs.
 *
 * Closed: a 4-line log tail in the top-right corner. Open: a panel covering
 * the bottom ~60% of the screen with a scrolling log, a 4×10 keyboard grid
 * navigated with the stick, and a one-line command prompt with a blinking
 * cursor. A types the highlighted cell, B backspaces, Start submits.
 *
 * The console reads the pad through kiln_input_get so a scripted tape
 * (jump ROMs, attract) can open it the same way a person does. Examples
 * gate gameplay logic on `kiln_console_is_open()` so the stick doesn't
 * leak into the player while the user is typing.
 *
 * Build flag: when `KILN_DEBUG` is undefined at compile time, the
 * `kiln_console_*` symbols are still defined (the module is always in
 * libkiln.a) but the example's `main.c` is expected to wrap its calls in
 * `#ifdef KILN_DEBUG` so a non-debug ROM pays zero cost.
 */
#ifndef KILN_CONSOLE_H
#define KILN_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A command the console can dispatch. `name` is matched case-insensitively
 *  against the first word the user types. `fn` is invoked with the parsed
 *  argv; argv[0] is the command name itself, like execv. */
typedef struct KilnConsoleCmd {
    const char *name;
    const char *help;
    void (*fn)(int argc, const char **argv);
} KilnConsoleCmd;

/** Clear the ring buffer and reset toggle/chord state. Idempotent. */
void kiln_console_init(void);

/** Register a table of commands. The pointer is held — caller keeps the
 *  storage alive. Multiple calls append; later registrations shadow earlier
 *  ones with the same name. */
void kiln_console_register(const KilnConsoleCmd *cmds, size_t n);

/** Append a line to the ring buffer, timestamped with the elapsed time since
 *  init in seconds.millis. Printf-style. Truncates at 63 chars. */
void kiln_console_log(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/** Per-frame: poll joypad, advance the chord state machine, and (when open)
 *  handle keyboard navigation and command submission. Call after the engine's
 *  `kiln_input_update()` so joypad state is fresh. `port` is 1-based to match
 *  libdragon's JOYPAD_PORT_1; pass 0 for the default (port 1). */
void kiln_console_update(int port);

/** Per-frame: draw the closed-state tail or the open-state full console.
 *  Call between `kiln_gui_end()` and `kiln_frame_end()`. */
void kiln_console_draw(void);

/** Run one command line as if it had been typed and submitted: echoed into
 *  the log as "> line", then dispatched exactly as the prompt dispatches it.
 *  For callers with a real keyboard — the browser launcher's Kiln Studio
 *  bridge, a test harness — rather than the pad-driven grid. Truncated at the
 *  prompt's 63 characters; an empty line does nothing. */
void kiln_console_exec(const char *line);

/** True while the console is open. Examples skip gameplay input parsing
 *  while this returns true so the player doesn't walk while the user types. */
bool kiln_console_is_open(void);

/** Read-only access to the ring buffer, oldest-first. Used by the panic
 *  surface to print recent log lines without duplicating storage. Returns the
 *  number of live lines (capped at CON_LOG_LINES). */
int kiln_console_tail_lines(void);
const char *kiln_console_tail_line(int idx); /* 0 = oldest live line */

#ifdef __cplusplus
}
#endif

#endif /* KILN_CONSOLE_H */