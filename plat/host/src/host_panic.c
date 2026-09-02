/* SPDX-License-Identifier: MIT
 *
 * host_panic.c — kiln_panic's two entry points, on the host.
 *
 * This is the one place plat/host implements a kiln_* function rather than a
 * libdragon or Tiny3D one, and the reason is that kiln_panic is not engine
 * logic in the first place: it is a CPU exception handler. The console version
 * reads VR4300 register state out of libdragon's exception_t, walks the stack
 * by hand (deliberately NOT via backtrace_foreach, whose own walk faults on
 * exactly the crashes worth catching — see kiln_panic.c's comment and the
 * hours it records losing to that), and halts.
 *
 * None of that has a host analogue. Shimming libdragon's exception_t would
 * mean inventing MIPS registers on x86, which is the definition of a fake. So
 * the host implements the same TWO-FUNCTION contract over signals instead, and
 * the two implementations share no logic because there is none to share.
 *
 * What this buys: a segfault in engine code under the host build prints where
 * it happened instead of dying silently. That is the whole argument for the
 * host target — neither of this project's boards has cartridge-side USB, so on
 * hardware every diagnostic is on-screen text and a VR4300 halt is diagnosed
 * after the fact with mips64-elf-addr2line.
 */
#include <kiln_panic.h>

/* execinfo.h is a glibc extension. musl (aarch64/riscv64 static builds) and
 * Emscripten do not have it, and its absence is a compile error rather than a
 * link failure, so it has to be tested for. Losing the backtrace degrades the
 * diagnostic to "which signal, at which address", which is still strictly more
 * than the console gives you. */
#if defined(__has_include)
#  if __has_include(<execinfo.h>)
#    define KILN_HAVE_EXECINFO 1
#  endif
#endif
#ifdef KILN_HAVE_EXECINFO
#  include <execinfo.h>
#endif

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_FRAMES 32

static char g_message[256];

static void write_all(const char *s)
{
    /* write(2), not printf: this runs from a signal handler, where stdio is
     * not reentrant. The console version has the same constraint for the same
     * reason — it is inside an exception handler. */
    const size_t n = strlen(s);
    ssize_t off = 0;
    while ((size_t)off < n) {
        ssize_t w = write(STDERR_FILENO, s + off, n - (size_t)off);
        if (w <= 0) return;
        off += w;
    }
}

static void handler(int sig, siginfo_t *info, void *ctx)
{
    (void)ctx;
    write_all("\n=== kiln panic ===\n");
    switch (sig) {
    case SIGSEGV: write_all("  SIGSEGV: invalid memory access\n"); break;
    case SIGBUS:  write_all("  SIGBUS: misaligned or unmapped access\n"); break;
    case SIGFPE:  write_all("  SIGFPE: arithmetic exception\n"); break;
    case SIGILL:  write_all("  SIGILL: illegal instruction\n"); break;
    case SIGABRT: write_all("  SIGABRT: assertion or abort\n"); break;
    default:      write_all("  signal\n"); break;
    }
    if (info && (sig == SIGSEGV || sig == SIGBUS)) {
        char buf[64];
        snprintf(buf, sizeof buf, "  fault address: %p\n", info->si_addr);
        write_all(buf);
    }
    if (g_message[0]) {
        write_all("  last message: ");
        write_all(g_message);
        write_all("\n");
    }

#ifdef KILN_HAVE_EXECINFO
    /* backtrace_symbols_fd is the async-signal-safe half of the pair;
     * backtrace_symbols() allocates and must not be called here. */
    void *frames[MAX_FRAMES];
    const int n = backtrace(frames, MAX_FRAMES);
    write_all("  backtrace:\n");
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
#else
    write_all("  backtrace: unavailable (no execinfo.h on this libc)\n");
#endif

    /* Halt rather than return, matching the console: continuing from an
     * unhandled exception corrupts state in ways that produce a second,
     * unrelated crash somewhere else. _exit and not exit, again for
     * reentrancy. */
    _exit(70);
}

void kiln_panic_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    const int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++)
        sigaction(sigs[i], &sa, NULL);
}

void kiln_panic_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_message, sizeof g_message, fmt, ap);
    va_end(ap);
}
