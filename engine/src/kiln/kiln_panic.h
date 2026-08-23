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
 *   - the panic message (from assertf or from kiln_panic_message)
 *   - the last several lines from kiln_console's ring buffer
 *   - the faulting PC, bad virtual address, cause, and a few GPRs
 *   - then chains to libdragon's exception_default_handler for the full dump.
 *
 * kiln_panic_install() is called automatically from kiln_engine_init(), so every
 * ROM that initializes the engine gets the panic surface. It is safe to call
 * even if the debug console was not initialized.
 */
#ifndef KILN_PANIC_H
#define KILN_PANIC_H

#ifdef __cplusplus
extern "C" {
#endif

/** Install the exception handler. Idempotent. */
void kiln_panic_install(void);

/** Append a message to the panic dump. Safe to call from normal code before
 *  a deliberate abort; not safe from interrupt/exception context. */
void kiln_panic_message(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif /* KILN_PANIC_H */