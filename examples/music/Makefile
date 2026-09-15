# SPDX-License-Identifier: MIT
#
# Two tracks through kiln_audio — an XM64 module on the RSP mixer and a looping
# VADPCM bed — under a visualiser of what the mixer is actually doing.
#
# Both are staged into filesystem/ by mkN64Rom's `assets` argument (mkMusic
# converts the .xm, mkSound the .wav), so there are no converter rules here.

BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

# Tiny3D's headers use C23-isms, so the dialect has to match.
N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: music.z64

# The filesystem, including subdirectories (see examples/clip-demo/Makefile):
# the assets land in music/ and sfx/, and a flat wildcard would miss both.
$(BUILD_DIR)/music.dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
music.z64: $(BUILD_DIR)/music.dfs

$(BUILD_DIR)/music.elf: $(OBJS)

music.z64: N64_ROM_TITLE = "Kiln Music"

clean:
	rm -rf $(BUILD_DIR) music.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
