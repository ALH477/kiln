# SPDX-License-Identifier: MIT
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: actors-demo.z64

$(BUILD_DIR)/actors-demo.elf: $(OBJS)

actors-demo.z64: N64_ROM_TITLE = "Kiln Actors"

clean:
	rm -rf $(BUILD_DIR) actors-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
