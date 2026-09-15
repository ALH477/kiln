# SPDX-License-Identifier: MIT
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: nightlight-demo.z64

# THE FILESYSTEM. Without these two lines n64.mk never attaches DragonFS
# and mkN64Rom's assets list is silently ignored. See splash-demo/Makefile.
$(BUILD_DIR)/nightlight-demo.dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
nightlight-demo.z64: $(BUILD_DIR)/nightlight-demo.dfs

$(BUILD_DIR)/nightlight-demo.elf: $(OBJS)

nightlight-demo.z64: N64_ROM_TITLE = "Kiln Nightlight"

clean:
	rm -rf $(BUILD_DIR) nightlight-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
