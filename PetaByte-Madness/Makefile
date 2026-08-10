# SPDX-License-Identifier: MPL-2.0
#
# PetaByte Madness — a standalone game flake output, alongside game/
# (Ganja Goblin) rather than under examples/.
#
# Include order: n64.mk, then t3d.mk, then m64.mk. Each prepends its own -l
# to N64_LDFLAGS, so the last one included ends up leftmost on the link line
# — which is what we want, since libm64 calls into Tiny3D, which calls into
# libdragon.
BUILD_DIR = build
SOURCE_DIR = src
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/m64.mk

# Tiny3D's headers use C23-isms, so the dialect has to match.
N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o \
       $(BUILD_DIR)/pm_veil.o \
       $(BUILD_DIR)/pm_demons.o \
       $(BUILD_DIR)/pm_hud.o \
       $(BUILD_DIR)/pm_models.o \
       $(BUILD_DIR)/pm_demo.o \
       $(BUILD_DIR)/pm_screens.o \
       $(BUILD_DIR)/pm_actors.o \
       $(BUILD_DIR)/pm_lab.o \
       $(BUILD_DIR)/pm_fx.o \
       $(BUILD_DIR)/pm_arrival.o \
       $(BUILD_DIR)/pm_sfx.o \
       $(BUILD_DIR)/pm_music.o \
       $(BUILD_DIR)/pm_env.o \
       $(BUILD_DIR)/pm_intake.o \
       $(BUILD_DIR)/pm_debug.o

all: petabyte-madness.z64

# THE FILESYSTEM. Without these two lines n64.mk never builds or attaches a
# DragonFS, and the ROM ships with NO filesystem at all — every rom:/ path
# then fails, which surfaces as libdragon asserting "File not found" on the
# first asset and dying on a black screen.
#
# This Makefile was copied from game/Makefile (Ganja Goblin), which ships no
# assets, so the omission was invisible until this game started shipping
# them. If you add `assets` to a mkN64Rom call, add these lines too;
# examples/assets-demo/Makefile is the reference.
$(BUILD_DIR)/petabyte-madness.dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
$(BUILD_DIR)/petabyte-madness.elf: $(OBJS)

petabyte-madness.z64: N64_ROM_TITLE = "PetaByte Madness"
petabyte-madness.z64: $(BUILD_DIR)/petabyte-madness.dfs

clean:
	rm -rf $(BUILD_DIR) petabyte-madness.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
