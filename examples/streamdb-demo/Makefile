# SPDX-License-Identifier: MPL-2.0
#
# Phase C verification: same three asset kinds as assets-demo, but loaded
# from a single StreamDB container instead of loose DFS files. The
# .streamdb is staged into filesystem/ by mkN64Rom's `assets` argument
# before this Makefile runs (nix/assets.nix's mkStreamdb), so there is no
# pack step here — same convention as examples/audio and examples/assets-demo.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/m64.mk
include $(N64_INST)/include/streamdb.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: streamdb-demo.z64

$(BUILD_DIR)/streamdb-demo.dfs: $(wildcard filesystem/*)
$(BUILD_DIR)/streamdb-demo.elf: $(OBJS)

streamdb-demo.z64: N64_ROM_TITLE = "M64 StreamDB"
streamdb-demo.z64: $(BUILD_DIR)/streamdb-demo.dfs

clean:
	rm -rf $(BUILD_DIR) streamdb-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean