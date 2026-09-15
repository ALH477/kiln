# SPDX-License-Identifier: MIT
#
# Live Faust voice + baked instrument comparison.
#
# Links the ks-voice object file (mkFaustVoice output) alongside the
# ks-baked wav64, and plays both: the live voice rendered on the VR4300, the
# baked sample through the RSP mixer, at the same pitch.

BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

# Tiny3D's headers use C23-isms, so the dialect has to match.
N64_CFLAGS += -std=gnu2x -Wno-error

# The Faust voice object file, passed in via FAUST_VOICE make flag.
# flake.nix sets: makeFlags = [ "FAUST_VOICE=${ks-voice}/lib/ksvoice.o" ]
FAUST_VOICE ?= $(error FAUST_VOICE must point to the ks-voice .o file)

OBJS = $(BUILD_DIR)/main.o $(FAUST_VOICE)

all: live-voice.z64

# The filesystem, including subdirectories (see examples/clip-demo/Makefile).
$(BUILD_DIR)/live-voice.dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
live-voice.z64: $(BUILD_DIR)/live-voice.dfs

$(BUILD_DIR)/live-voice.elf: $(OBJS)

live-voice.z64: N64_ROM_TITLE = "Kiln Live Voice"

clean:
	rm -rf $(BUILD_DIR) live-voice.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
