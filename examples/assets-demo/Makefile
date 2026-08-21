# SPDX-License-Identifier: MPL-2.0
#
# The model, sprite and sound are staged into filesystem/ by mkN64Rom's
# `assets` argument before this Makefile runs (nix/assets.nix), so there is
# no gltf_to_t3d/mksprite/audioconv64 rule here — see examples/audio's
# Makefile for the same convention applied to baked audio.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: assets-demo.z64

$(BUILD_DIR)/assets-demo.dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
$(BUILD_DIR)/assets-demo.elf: $(OBJS)

assets-demo.z64: N64_ROM_TITLE = "Kiln Assets"
assets-demo.z64: $(BUILD_DIR)/assets-demo.dfs

clean:
	rm -rf $(BUILD_DIR) assets-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
