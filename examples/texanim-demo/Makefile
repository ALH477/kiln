# SPDX-License-Identifier: MIT
#
# texanim-demo: exercises kiln_texanim + kiln_vanim.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: texanim-demo.z64

# THE FILESYSTEM: the models and sprites from mkN64Rom's `assets` list. Without
# these two lines n64.mk never builds a DragonFS and every rom:/ path fails.
$(BUILD_DIR)/texanim-demo.dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
texanim-demo.z64: $(BUILD_DIR)/texanim-demo.dfs

$(BUILD_DIR)/texanim-demo.elf: $(OBJS)

texanim-demo.z64: N64_ROM_TITLE = "Kiln TexAnim"

clean:
	rm -rf $(BUILD_DIR) texanim-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean