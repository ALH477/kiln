# SPDX-License-Identifier: MIT
#
# Include order matters: n64.mk, then t3d.mk, then kiln.mk. Each prepends its
# own -l to N64_LDFLAGS, so the last one included ends up leftmost on the
# link line — which is what we want, since libkiln calls into Tiny3D, which
# calls into libdragon.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

# Tiny3D's headers use C23-isms, so the dialect has to match.
N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: engine.z64

$(BUILD_DIR)/engine.elf: $(OBJS)

engine.z64: N64_ROM_TITLE = "Kiln Engine"

clean:
	rm -rf $(BUILD_DIR) engine.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
