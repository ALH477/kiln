/* SPDX-License-Identifier: MIT
 *
 * Forge/selftest — does this flashcart let a ROM write to its SD card?
 *
 * The first thing to run on an unfamiliar cart, and specifically on the ED64
 * Plus this tooling targets. Everything Forge does rests on one assumption:
 * that `kiln_store`'s SD backend works on a board libcart claims to support but
 * which is a third-party clone of a twenty-year-old design. That is answerable
 * in one boot, so it gets answered before a line of editor code depends on it.
 *
 * ── Why libdragon's console and not kiln_gui ────────────────────────────
 *
 * A probe wants a scrolling log, not a frame. `console_init` + printf gives one
 * for free, brings in no Tiny3D, and — the part that matters — cannot itself be
 * the reason the screen is blank. kiln_gui would put the engine's whole frame
 * bracket between a failure and the report of it. The rule this follows is the
 * one the n64-verify skill states for the debug overlay: the diagnostic must be
 * on screen, because on a cart with no USB `debugf()` goes nowhere at all.
 *
 * ── What it exercises ──────────────────────────────────────────────────
 *
 * kiln_store_selftest(), i.e. the same code the editor saves through — not a
 * parallel implementation. A probe that proves a copy of the write path proves
 * nothing about the write path.
 */
#include <libdragon.h>
#include <kiln/kiln_store.h>

static void log_line(void *ctx, const char *line)
{
    (void)ctx;
    printf("  %s\n", line);
    console_render();
}

int main(void)
{
    console_init();
    console_set_render_mode(RENDER_MANUAL);

    printf("KILN FORGE - store probe\n\n");
    console_render();

    /* Ask for the best backend and report what we actually got. On an emulator
     * this lands on `rom` and the probe correctly reports that there is nothing
     * to write to — which is information, not a failure of the ROM. */
    KilnStoreKind kind = kiln_store_init(KILN_STORE_CART_SD);

    int fails = kiln_store_selftest(log_line, NULL);

    printf("\n");
    printf("bus %s\n", kiln_store_bus_name());
    printf("\n");

    /* Name the backend that actually passed. The first version of this said
     * "SD writes work on this cart" unconditionally, and printed it after a
     * round trip through the 32 KB save chip on a machine with no SD card at
     * all. A probe that reports the wrong medium is worse than one that fails,
     * because it is believed. */
    if (fails == 0 && kind == KILN_STORE_CART_SD) {
        printf("PASS - SD writes work on this cart.\n");
        printf("Power off, put the card in a PC, and look\n");
        printf("for FORGE/PROBE.FRG and FORGE/PROBE.TXT.\n");
    } else if (fails == 0) {
        printf("PASS - but on the %s backend, not SD.\n",
               kiln_store_kind_name());
        printf("Forge will work; getting a level to a PC needs\n");
        printf("the save-chip flush (hold RESET about 2 s) or\n");
        printf("the on-screen dump. Under an emulator this is\n");
        printf("the expected result: there is no SD card.\n");
    } else {
        printf("FAIL - %d step(s) failed.\n", fails);
        printf("Forge will fall back to the 32 KB save chip\n");
        printf("(hold RESET about 2 s to flush it to the card)\n");
        printf("or to the on-screen dump. Neither needs this\n");
        printf("to work; both are slower.\n");
    }

    /* Unmount before the user is told it is safe to power off. A FatFs volume
     * with buffered metadata and a yanked card loses the ROM sitting next to
     * the level. */
    kiln_store_close();
    printf("\nvolume unmounted - safe to power off.\n");
    console_render();

    while (1) { /* park: this ROM's whole output is the text above */ }
}
