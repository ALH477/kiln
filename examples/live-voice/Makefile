# SPDX-License-Identifier: MIT
#
# Live Faust voice + baked instrument comparison.
#
# Links the ks-voice object file (mkFaustVoice output) alongside the
# ks-baked wav64, and plays both: the live voice on A, the baked sample on B.
# This is the end-to-end proof that the live synthesis path works on-console.

BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/kiln.mk

# The Faust voice object file, passed in via FAUST_VOICE make flag.
# flake.nix sets: makeFlags = [ "FAUST_VOICE=${ks-voice}/lib/ksvoice.o" ]
FAUST_VOICE ?= $(error FAUST_VOICE must point to the ks-voice .o file)

OBJS = $(BUILD_DIR)/main.o $(FAUST_VOICE)

all: live-voice.z64

$(BUILD_DIR)/live-voice.dfs: $(wildcard filesystem/*)
$(BUILD_DIR)/live-voice.elf: $(OBJS)

live-voice.z64: N64_ROM_TITLE = "Kiln Live Voice"
live-voice.z64: $(BUILD_DIR)/live-voice.dfs

clean:
	rm -rf $(BUILD_DIR) live-voice.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean