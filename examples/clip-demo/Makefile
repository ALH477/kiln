# SPDX-License-Identifier: MPL-2.0
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/m64.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: clip-demo.z64

$(BUILD_DIR)/clip-demo.elf: $(OBJS)

clip-demo.z64: N64_ROM_TITLE = "M64 Clip"

clean:
	rm -rf $(BUILD_DIR) clip-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean