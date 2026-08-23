# SPDX-License-Identifier: MIT
#
# Cinematic-demo ROM: one continuous 60-second scene that exercises every
# engine subsystem in a single shot. Interceptor in a hangar; goblin captain
# walking the perimeter (kiln_player + kiln_skel walk-cycle); droids orbiting
# the ship; aliens approaching (kiln_target acquire); physics-driven crates
# the goblin bumps; music + SFX; door opens on a delayed event.
#
# Same shape as examples/interceptor-demo's Makefile. The full asset list
# lives in flake.nix's `cinematic-demo` derivation.

BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: cinematic-demo.z64

$(BUILD_DIR)/cinematic-demo.elf: $(OBJS)

cinematic-demo.z64: N64_ROM_TITLE = "Kiln Cinematic"
cinematic-demo.z64: $(BUILD_DIR)/cinematic-demo.dfs

clean:
	rm -rf $(BUILD_DIR) cinematic-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
