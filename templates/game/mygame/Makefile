# SPDX-License-Identifier: MIT
#
# A Kiln game's build: libdragon's n64.mk, then Tiny3D's t3d.mk, then Kiln's
# kiln.mk. Each prepends its own -l, so the last included ends up leftmost on
# the link line — libkiln calls Tiny3D, which calls libdragon.
#
# N64_INST and N64_GCCPREFIX come from mkN64Rom (or `nix develop` in Kiln).
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

# Tiny3D's headers use C23-isms, so the dialect has to match.
N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: mygame.z64

$(BUILD_DIR)/mygame.elf: $(OBJS)

mygame.z64: N64_ROM_TITLE = "My Game"

clean:
	rm -rf $(BUILD_DIR) mygame.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
