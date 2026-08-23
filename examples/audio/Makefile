# SPDX-License-Identifier: MIT
#
# Plays a VADPCM-baked instrument through libdragon's RSP mixer.
#
# The .wav64 is staged into filesystem/ by mkN64Rom's `assets` argument before
# this Makefile runs, so there is no audioconv64 rule here — the encoding
# already happened in mkBakedInstrument, at full quality, on the host.
# Outside Nix you would add the audioconv64 rule from libdragon's
# examples/audioplayer/Makefile instead.

BUILD_DIR = build
include $(N64_INST)/include/n64.mk

OBJS = $(BUILD_DIR)/audio.o

all: audio.z64

$(BUILD_DIR)/audio.dfs: $(wildcard filesystem/*)
$(BUILD_DIR)/audio.elf: $(OBJS)

audio.z64: N64_ROM_TITLE = "Kiln Audio"
audio.z64: $(BUILD_DIR)/audio.dfs

clean:
	rm -rf $(BUILD_DIR) audio.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
