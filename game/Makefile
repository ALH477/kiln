# SPDX-License-Identifier: MPL-2.0
#
# Ganja Goblin — a standalone game flake output (not an examples/ entry).
# Include order: n64.mk, then t3d.mk, then m64.mk. Each prepends its own -l to
# N64_LDFLAGS, so the last one included ends up leftmost on the link line —
# which is what we want, since libm64 calls into Tiny3D, which calls into
# libdragon.
BUILD_DIR = build
SOURCE_DIR = src
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/m64.mk

# Tiny3D's headers use C23-isms, so the dialect has to match.
N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o \
       $(BUILD_DIR)/gg_board1.o \
       $(BUILD_DIR)/gg_board2.o \
       $(BUILD_DIR)/gg_boards.o \
       $(BUILD_DIR)/gg_spaces.o \
       $(BUILD_DIR)/gg_buds.o \
       $(BUILD_DIR)/gg_turn.o \
       $(BUILD_DIR)/gg_goblins.o \
       $(BUILD_DIR)/gg_specials.o \
       $(BUILD_DIR)/gg_screens.o \
       $(BUILD_DIR)/gg_hud.o

all: ganja-goblin.z64

$(BUILD_DIR)/ganja-goblin.elf: $(OBJS)

ganja-goblin.z64: N64_ROM_TITLE = "Ganja Goblin"

clean:
	rm -rf $(BUILD_DIR) ganja-goblin.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean