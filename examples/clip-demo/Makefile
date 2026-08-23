# SPDX-License-Identifier: MIT
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: clip-demo.z64

# THE FILESYSTEM. Without these two lines n64.mk never builds or attaches a
# DragonFS and this ROM's mkN64Rom `assets` list is SILENTLY IGNORED — the .z64
# comes out byte-for-byte the size of the assetless build and every rom:/ path
# fails at runtime. That is what was happening here: the assets have never been
# in this ROM. Found by nix/rom.nix's gate, which now fails the build instead.
# See Forge/Makefile's "THE FILESYSTEM" comment for the same story.
$(BUILD_DIR)/clip-demo.dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
clip-demo.z64: $(BUILD_DIR)/clip-demo.dfs

$(BUILD_DIR)/clip-demo.elf: $(OBJS)

clip-demo.z64: N64_ROM_TITLE = "Kiln Clip"

clean:
	rm -rf $(BUILD_DIR) clip-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean