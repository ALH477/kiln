/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_console.h — retro on-screen debug console.
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
 * The console polls libdragon's joypad directly (not the engine's m64_input
 * wrapper) so it stays usable when the game's input layer is paused or
 * rebound. Examples gate gameplay logic on `m64_console_is_open()` so the
 * stick doesn't leak into the player while the user is typing.
 *
 * Build flag: when `M64_DEBUG` is undefined at compile time, the
 * `m64_console_*` symbols are still defined (the module is always in
 * libm64.a) but the example's `main.c` is expected to wrap its calls in
 * `#ifdef M64_DEBUG` so a non-debug ROM pays zero cost.
 */
#ifndef M64_CONSOLE_H
#define M64_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A command the console can dispatch. `name` is matched case-insensitively
 *  against the first word the user types. `fn` is invoked with the parsed
 *  argv; argv[0] is the command name itself, like execv. */
typedef struct M64ConsoleCmd {
    const char *name;
    const char *help;
    void (*fn)(int argc, const char **argv);
} M64ConsoleCmd;

/** Clear the ring buffer and reset toggle/chord state. Idempotent. */
void m64_console_init(void);

/** Register a table of commands. The pointer is held — caller keeps the
 *  storage alive. Multiple calls append; later registrations shadow earlier
 *  ones with the same name. */
void m64_console_register(const M64ConsoleCmd *cmds, size_t n);

/** Append a line to the ring buffer, timestamped with the elapsed time since
 *  init in seconds.millis. Printf-style. Truncates at 63 chars. */
void m64_console_log(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/** Per-frame: poll joypad, advance the chord state machine, and (when open)
 *  handle keyboard navigation and command submission. Call after the engine's
 *  `m64_input_update()` so joypad state is fresh. `port` is 1-based to match
 *  libdragon's JOYPAD_PORT_1; pass 0 for the default (port 1). */
void m64_console_update(int port);

/** Per-frame: draw the closed-state tail or the open-state full console.
 *  Call between `m64_gui_end()` and `m64_frame_end()`. */
void m64_console_draw(void);

/** True while the console is open. Examples skip gameplay input parsing
 *  while this returns true so the player doesn't walk while the user types. */
bool m64_console_is_open(void);

/** Read-only access to the ring buffer, oldest-first. Used by the panic
 *  surface to print recent log lines without duplicating storage. Returns the
 *  number of live lines (capped at CON_LOG_LINES). */
int m64_console_tail_lines(void);
const char *m64_console_tail_line(int idx); /* 0 = oldest live line */

#ifdef __cplusplus
}
#endif

#endif /* M64_CONSOLE_H */