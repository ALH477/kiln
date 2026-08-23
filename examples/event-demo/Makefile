# SPDX-License-Identifier: MIT
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: event-demo.z64

$(BUILD_DIR)/event-demo.elf: $(OBJS)

event-demo.z64: N64_ROM_TITLE = "Kiln Event"

clean:
	rm -rf $(BUILD_DIR) event-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean