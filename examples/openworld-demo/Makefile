# SPDX-License-Identifier: MIT
#
# Include order matters: n64.mk, then t3d.mk, then streamdb.mk, then
# kiln.mk. Each prepends its own -l to N64_LDFLAGS, so the last one included
# ends up leftmost on the link line — which is what we want, since libkiln's
# kiln_asset.o (the streaming pacer's I/O half) calls into libstreamdb_emb,
# and streamdb_io_dfs.o calls back into libdragon. Same order and reasoning
# as examples/streamdb-demo/Makefile.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/streamdb.mk
include $(N64_INST)/include/kiln.mk

# Tiny3D's headers use C23-isms, so the dialect has to match.
N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: openworld-demo.z64

$(BUILD_DIR)/openworld-demo.dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
$(BUILD_DIR)/openworld-demo.elf: $(OBJS)

openworld-demo.z64: N64_ROM_TITLE = "Kiln Open World"
openworld-demo.z64: $(BUILD_DIR)/openworld-demo.dfs

clean:
	rm -rf $(BUILD_DIR) openworld-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean