# SPDX-License-Identifier: MIT
#
# The model and sound are staged into filesystem/ by mkN64Rom's `assets`
# argument before this Makefile runs (nix/assets.nix) — same convention as
# examples/assets-demo.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: camera-skel-demo.z64

$(BUILD_DIR)/camera-skel-demo.dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
$(BUILD_DIR)/camera-skel-demo.elf: $(OBJS)

camera-skel-demo.z64: N64_ROM_TITLE = "Kiln Courtyard"
camera-skel-demo.z64: $(BUILD_DIR)/camera-skel-demo.dfs

clean:
	rm -rf $(BUILD_DIR) camera-skel-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
