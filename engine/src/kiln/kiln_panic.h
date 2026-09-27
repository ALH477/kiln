/* SPDX-License-Identifier: MIT
 *
 * kiln_panic.h — on-screen crash / assertion surface.
 *
 * Intercepts libdragon exceptions (including those raised by `assertf`) and
 * prints a readable dump via libdragon's text console before halting. This is
 * safe even when the RDP is in a bad state, because it uses the CPU-driven
 * text console rather than rdpq.
 *
 * The handler prints:
 *   - the panic message (from assertf or from fig_panic_message)
 *   - the last several lines from fig_console's ring buffer
 *   - the faulting PC, bad virtual address, cause, and a few GPRs
 *   - then chains to libdragon's exception_default_handler for the full dump.
 *
 * fig_panic_install() is called automatically from fig_engine_init(), so every
 * ROM that initializes the engine gets the panic surface. It is safe to call
 * even if the debug console was not initialized.
 */
#ifndef FIG_PANIC_H
#define FIG_PANIC_H


/* The prefix migration train (docs/NAMING.md section 9 step 2). Pulled in by
 * every public header (a quoted include, so it resolves both in this tree and
 * in the installed include/kiln prefix) rather than force-included by
 * kiln-inst.mk, because a
 * force-include only reaches builds that include that file — a Nix check or a
 * host build compiling a downstream's sources directly never saw it, and
 * PetaByte-Madness' pm-cine check is what proved that. Deleting the train is
 * still a scripted one-line removal from these headers plus the file itself.
 */
#include "kiln_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Install the exception handler. Idempotent. */
void fig_panic_install(void);

/** Append a message to the panic dump. Safe to call from normal code before
 *  a deliberate abort; not safe from interrupt/exception context. */
void fig_panic_message(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif /* FIG_PANIC_H */