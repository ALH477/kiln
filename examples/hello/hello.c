// SPDX-License-Identifier: MIT
//
// The smallest thing that proves the whole path works: cross toolchain ->
// libdragon -> n64tool -> a .z64 that boots. If this displays text on real
// hardware and in Ares, M1 is done.

#include <libdragon.h>

int main(void)
{
    console_init();
    console_set_render_mode(RENDER_MANUAL);

    // debugf() goes to the host over flashcart USB when one is attached, and
    // is a no-op otherwise. This is the channel `./dev debug` reads.
    debug_init_isviewer();
    debugf("Kiln: hello from the VR4300\n");

    while (1) {
        console_clear();
        printf("\n  Kiln\n\n");
        printf("  libdragon on a Nix-built mips64-elf toolchain.\n");
        console_render();
    }
}
