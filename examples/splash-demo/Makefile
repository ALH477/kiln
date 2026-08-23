# SPDX-License-Identifier: MIT
#
# The kiln model is staged into filesystem/models/ by mkN64Rom's `assets`
# argument before this Makefile runs (nix/assets.nix's mkModel), so there
# is no gltf_to_t3d rule here.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: splash-demo.z64

# THE FILESYSTEM. Without these two lines n64.mk never builds or attaches a
# DragonFS and this ROM's mkN64Rom `assets` list is SILENTLY IGNORED — see
# examples/map-demo/Makefile's comment for the whole story and the gate
# (nix/rom.nix) that now catches it instead.
$(BUILD_DIR)/splash-demo.dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
splash-demo.z64: $(BUILD_DIR)/splash-demo.dfs

$(BUILD_DIR)/splash-demo.elf: $(OBJS)

splash-demo.z64: N64_ROM_TITLE = "Kiln Splash"

clean:
	rm -rf $(BUILD_DIR) splash-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
