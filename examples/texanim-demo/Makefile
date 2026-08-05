# SPDX-License-Identifier: MPL-2.0
#
# texanim-demo: exercises m64_texanim + m64_vanim.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/m64.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: texanim-demo.z64

$(BUILD_DIR)/texanim-demo.elf: $(OBJS)

texanim-demo.z64: N64_ROM_TITLE = "M64 TexAnim"

clean:
	rm -rf $(BUILD_DIR) texanim-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean