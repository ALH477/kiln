# SPDX-License-Identifier: MIT
#
# XM64 tracker music playback example.
#
# The .xm64 is staged into filesystem/ by mkN64Rom's `assets` argument
# (mkMusic converts the .xm via audioconv64). This ROM loads and plays
# it through libdragon's XM64 player, which uses the RSP mixer.

BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/kiln.mk

OBJS = $(BUILD_DIR)/main.o

all: music.z64

$(BUILD_DIR)/music.dfs: $(wildcard filesystem/*)
$(BUILD_DIR)/music.elf: $(OBJS)

music.z64: N64_ROM_TITLE = "Kiln Music"
music.z64: $(BUILD_DIR)/music.dfs

clean:
	rm -rf $(BUILD_DIR) music.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean