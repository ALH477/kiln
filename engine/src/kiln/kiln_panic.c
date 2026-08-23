/* SPDX-License-Identifier: MIT
 *
 * kiln_panic.c — see kiln_panic.h for the design.
 *
 * Uses libdragon's text console (console.h), not rdpq, because an exception
 * can fire while the RDP is mid-command and drawing through rdpq then is
 * undefined. The text console is CPU-driven and safe in this context.
 */
#include "kiln_panic.h"

#include <kiln/kiln_console.h>

#include <libdragon.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ── State ──────────────────────────────────────────────────────────────── */

static char g_message[256];
static int  g_message_len;
static exception_handler_t g_prev_handler;
static bool g_installed;

/* ── Message buffer ─────────────────────────────────────────────────────── */

void kiln_panic_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_message_len += vsnprintf(g_message + g_message_len,
                               sizeof(g_message) - g_message_len, fmt, ap);
    va_end(ap);
    if (g_message_len < 0) g_message_len = 0;
    if (g_message_len >= (int)sizeof(g_message)) g_message_len = sizeof(g_message) - 1;
}

/* ── Cause decoding ─────────────────────────────────────────────────────── */

/* VR4300 Cause register, bits 6:2. Names are the manual's mnemonics with a
 * plain-English gloss, because "AdEL" alone tells you nothing at 3am. */
static const char *exc_name(uint32_t cause, const char **gloss)
{
    switch ((cause >> 2) & 0x1F) {
    case 0:  *gloss = "interrupt";                  return "Int";
    case 1:  *gloss = "TLB modified";               return "Mod";
    case 2:  *gloss = "TLB miss on load";           return "TLBL";
    case 3:  *gloss = "TLB miss on store";          return "TLBS";
    case 4:  *gloss = "bad address on LOAD (misaligned or out of range)";
             return "AdEL";
    case 5:  *gloss = "bad address on STORE (misaligned or out of range)";
             return "AdES";
    case 6:  *gloss = "bus error, instruction fetch"; return "IBE";
    case 7:  *gloss = "bus error, data";            return "DBE";
    case 8:  *gloss = "syscall";                    return "Sys";
    case 9:  *gloss = "breakpoint";                 return "Bp";
    case 10: *gloss = "reserved instruction";       return "RI";
    case 11: *gloss = "coprocessor unusable";       return "CpU";
    case 12: *gloss = "arithmetic overflow";        return "Ov";
    case 13: *gloss = "trap";                       return "Tr";
    case 15: *gloss = "floating point (NaN, /0, or invalid op)";  return "FPE";
    case 23: *gloss = "watchpoint";                 return "WATCH";
    default: *gloss = "unknown";                    return "???";
    }
}

/* Is this address safe to DEREFERENCE from inside an exception handler?
 * Checked before every read in the stack walk below, because a fault here
 * is the double-fault that hid the real crash in the first place. */
static bool readable(uint32_t addr)
{
    if (addr & 3) return false;                      /* must be word aligned */
    /* Cached RDRAM only. 8 MB covers an Expansion Pak; a 4 MB console simply
     * never produces addresses above 0x80400000 for us to accept. */
    return addr >= 0x80000000u && addr < 0x80800000u;
}

/* A plausible return address: word-aligned and inside the cached RDRAM the
 * ROM is running from. Deliberately loose — this is a hint list for
 * addr2line, not a claim about the true call chain. */
static bool code_like(uint32_t addr)
{
    return (addr & 3) == 0 && addr >= 0x80000000u && addr < 0x80800000u;
}

/* ── Dump ───────────────────────────────────────────────────────────────── */

static void panic_dump(exception_t *ex)
{
    /* Initialise the text console. This is safe here: we are in an exception
     * context and the display may be in any state, but the text console writes
     * directly to the framebuffer without touching the RDP command stream. */
    console_init();
    console_set_render_mode(RENDER_AUTOMATIC);
    console_set_debug(true);

    const uint32_t cause = (ex && ex->regs) ? (uint32_t)ex->regs->cr : 0;
    const uint32_t epc   = (ex && ex->regs) ? (uint32_t)ex->regs->epc : 0;
    const uint32_t bad   = (uint32_t)C0_BADVADDR();
    const char *gloss = "";
    const char *code = exc_name(cause, &gloss);

    /* ── The terse summary ──────────────────────────────────────────────
     * One line, at the top, before anything that could fail. If the dump
     * dies half way through, this is the line that survived — so it has to
     * carry the two facts that actually locate a crash: what went wrong
     * and where. */
    printf("\n=== KILN PANIC ===\n");
    printf("%s @ %08lx", code, (unsigned long)epc);
    if (((cause >> 2) & 0x1F) == 4 || ((cause >> 2) & 0x1F) == 5)
        printf("  touching %08lx", (unsigned long)bad);
    printf("\n%s\n\n", gloss);

    if (g_message_len > 0)
        printf("message:\n%s\n\n", g_message);

    if (ex && ex->regs) {
        const volatile reg_block_t *r = ex->regs;

        printf("cause %08lx  sr %08lx  badvaddr %08lx\n",
               (unsigned long)cause, (unsigned long)r->sr,
               (unsigned long)bad);
        printf("epc   %08lx  ra %08lx  sp       %08lx\n",
               (unsigned long)epc, (unsigned long)r->gpr[31],
               (unsigned long)r->gpr[29]);

        /* Floating point status, when it is the FPU that complained. The
         * flag bits are what separate "divided by zero" from "did maths on
         * a NaN", which are very different bugs with the same panic. */
        if (((cause >> 2) & 0x1F) == 15) {
            const unsigned long f = (unsigned long)r->fc31;
            printf("fcr31 %08lx =", f);
            if (f & (1 << 16)) printf(" UNIMPL");
            if (f & (1 << 17)) printf(" INVALID");
            if (f & (1 << 18)) printf(" DIV0");
            if (f & (1 << 19)) printf(" OVERFLOW");
            if (f & (1 << 20)) printf(" UNDERFLOW");
            if (f & (1 << 21)) printf(" INEXACT");
            printf("\n");
        }

        /* Argument and temp registers: the ones that usually hold the bad
         * pointer. Four per line to fit 320x240. */
        static const char *const NAME[] = {
            "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3",
            "s0", "s1", "s2", "s3", "v0", "v1", "gp", "fp",
        };
        static const int IDX[] = { 4, 5, 6, 7, 8, 9, 10, 11,
                                   16, 17, 18, 19, 2, 3, 28, 30 };
        printf("\nregisters:\n");
        for (int i = 0; i < 16; i += 4) {
            printf(" ");
            for (int j = 0; j < 4; j++)
                printf(" %s %08lx", NAME[i + j],
                       (unsigned long)r->gpr[IDX[i + j]]);
            printf("\n");
        }

        /* ── Guarded stack walk ─────────────────────────────────────────
         * NOT libdragon's backtrace_foreach. That walks frame pointers and
         * dereferences whatever it finds, so on a corrupted stack it faults
         * — which is precisely how the real exception got hidden behind a
         * second one. This instead scans upward from sp and only reads
         * addresses it has already proved are aligned and inside RDRAM, so
         * it cannot fault however wrecked the stack is.
         *
         * The cost of that safety is precision: these are CANDIDATES, not a
         * call chain. Resolve them with
         *   mips64-none-elf-addr2line -f -C -e <rom>.elf <addr>
         * and read from the top — the first few that resolve to your own
         * code are almost always the path in. */
        const uint32_t sp = (uint32_t)r->gpr[29];
        printf("\nstack candidates (sp %08lx):\n", (unsigned long)sp);
        int shown = 0;
        for (uint32_t a = sp; a < sp + 0x400 && shown < 12; a += 4) {
            if (!readable(a)) continue;
            const uint32_t w = *(volatile uint32_t *)a;
            if (!code_like(w)) continue;
            printf("  %08lx: %08lx\n", (unsigned long)a, (unsigned long)w);
            shown++;
        }
        if (!shown) printf("  (none — stack is unreadable or empty)\n");
    }

    /* Print the last log lines from the console ring buffer, oldest first. */
    int n = kiln_console_tail_lines();
    if (n > 0) {
        printf("\nrecent log:\n");
        for (int i = 0; i < n; i++)
            printf("  %s\n", kiln_console_tail_line(i));
    }

    printf("\nhalting.\n");
}

/* ── Handler ───────────────────────────────────────────────────────────── */

static void panic_handler(exception_t *ex)
{
    /* ── Re-entrancy guard: the first report wins ───────────────────────
     * A crash inside the crash handler used to overwrite the screen with
     * ITSELF, and the second report is always the less interesting one —
     * it describes the dump machinery, not the bug.
     *
     * That is not hypothetical. This handler used to end by chaining to
     * exception_default_handler, whose backtrace walks frame pointers and
     * dereferences them; on a corrupted stack it raised AdEL, re-entered
     * here, and printed "Misaligned read @ backtrace_foreach" over the
     * genuine "Floating point invalid operation" that had just been
     * displayed. Hours went into the wrong exception.
     *
     * So: once, ever. A second entry prints one line under the first
     * report and stops. */
    static volatile int in_panic;
    if (in_panic) {
        printf("\n[secondary fault %08lx during dump - report above is the "
               "real one]\n",
               (unsigned long)(ex && ex->regs ? ex->regs->epc : 0));
        while (1) { }
    }
    in_panic = 1;

    panic_dump(ex);

    /* Deliberately does NOT chain to exception_default_handler.
     *
     * Its backtrace is the thing that faults on exactly the crashes worth
     * debugging — stack corruption, blown stacks, wild pointers — so
     * chaining trades the real diagnosis for a prettier one that only
     * works when the stack is already healthy. The guarded scan in
     * panic_dump covers the same ground without being able to fault.
     *
     * Halt here rather than return: continuing from an unhandled exception
     * re-executes the faulting instruction and loops forever, which on
     * screen is indistinguishable from a hang. */
    while (1) { }
}

void kiln_panic_install(void)
{
    if (g_installed) return;
    g_installed = true;
    g_prev_handler = register_exception_handler(panic_handler);
}