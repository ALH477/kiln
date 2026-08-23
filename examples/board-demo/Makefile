# SPDX-License-Identifier: MIT
#
# board-demo: kiln_rng + kiln_dice + kiln_board + kiln_turn end-to-end proof.
# Four tokens advance around a branching graph by die rolls, taking turns
# across rounds. No 3D, no assets — the proof is the topology + state
# machine, not rendering.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: board-demo.z64

$(BUILD_DIR)/board-demo.elf: $(OBJS)

board-demo.z64: N64_ROM_TITLE = "Kiln Board"

clean:
	rm -rf $(BUILD_DIR) board-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean