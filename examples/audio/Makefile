# SPDX-License-Identifier: MIT
#
# The baked path played as an instrument: one VADPCM pluck, retuned per note by
# the RSP mixer, under a 3D visualiser.
#
# The .wav64 is staged into filesystem/ by mkN64Rom's `assets` argument before
# this Makefile runs, so there is no audioconv64 rule here — the encoding
# already happened in mkBakedInstrument, at full quality, on the host.

BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

# Tiny3D's headers use C23-isms, so the dialect has to match.
N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/audio.o

all: audio.z64

# The filesystem, including subdirectories (see examples/clip-demo/Makefile).
$(BUILD_DIR)/audio.dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
audio.z64: $(BUILD_DIR)/audio.dfs

$(BUILD_DIR)/audio.elf: $(OBJS)

audio.z64: N64_ROM_TITLE = "Kiln Audio"

clean:
	rm -rf $(BUILD_DIR) audio.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
