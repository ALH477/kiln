# SPDX-License-Identifier: MIT
#
# Bass-synth ROM: a 4-controller collaborative bass synthesizer. 16
# single-cycle wavetables (4 engines x 4 variants) are baked as looping
# wav64s in flake.nix and pitched live by the RSP mixer; the VR4300 only
# does per-frame envelope + stick-morph updates for up to 12 voices.

BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: bass-synth.z64

$(BUILD_DIR)/bass-synth.elf: $(OBJS)

bass-synth.z64: N64_ROM_TITLE = "Kiln Bass Synth"
bass-synth.z64: $(BUILD_DIR)/bass-synth.dfs

clean:
	rm -rf $(BUILD_DIR) bass-synth.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean