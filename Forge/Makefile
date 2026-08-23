# SPDX-License-Identifier: MIT
#
# Forge — the on-console voxel level/cinematic editor.
#
# Include order n64.mk, t3d.mk, kiln.mk: each prepends its own -l, so the last
# included lands leftmost, which matches the dependency direction (libkiln calls
# Tiny3D calls libdragon).
BUILD_DIR = build
SOURCE_DIR = src
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

# Tiny3D's headers use C23-isms, so the dialect has to match.
N64_CFLAGS += -std=gnu2x -Wno-error

# Boot straight into one mode. `FORGE_MODE=WALK` becomes -DFORGE_BOOT_MODE=
# FORGE_MODE_WALK, which forge_main.c reads once at startup.
#
# This is the jump-ROM idiom (a downstream game's Makefile pioneered it) and
# it exists for the same reason: `./dev shot` has no input path at all by
# design, and `./dev
# drive`'s uinput->SDL->ares chain is fragile enough that a capture which needs a
# chord is a capture you cannot trust. WALK is reached by pressing L+R, so
# without this flag the one mode whose whole purpose is standing in the level
# could only ever be verified by hand.
#
# Appended to N64_CFLAGS here rather than passed as N64_CFLAGS+= on the make
# command line, for the reason nix/rom.nix's KILN_DEBUG comment spells out: a
# command-line N64_CFLAGS assignment OVERRIDES the one n64.mk built and silently
# drops every include path with it.
ifdef FORGE_MODE
  N64_CFLAGS += -DFORGE_BOOT_MODE=FORGE_MODE_$(FORGE_MODE)
endif

OBJS = $(BUILD_DIR)/forge_main.o \
       $(BUILD_DIR)/forge_cam.o \
       $(BUILD_DIR)/forge_geo.o \
       $(BUILD_DIR)/forge_walk.o \
       $(BUILD_DIR)/forge_hud.o \
       $(BUILD_DIR)/forge_paint.o \
       $(BUILD_DIR)/forge_ent.o \
       $(BUILD_DIR)/forge_light.o \
       $(BUILD_DIR)/forge_cine.o \
       $(BUILD_DIR)/forge_io.o

all: forge.z64

# THE FILESYSTEM. Without these two lines n64.mk never builds or attaches a
# DragonFS and a mkN64Rom `assets` list is SILENTLY IGNORED — the ROM comes out
# byte-for-byte the size of the assetless one. A downstream game shipped
# without a filesystem for a while because it hit this exact trap; Forge
# walked into it anyway, and the symptom was `.#forge-dfs` booting
# with `store none` and the seeded starter room instead of the baked level.
#
# Guarded on the directory existing, because `.#forge` ships no assets at all and
# mkdfs has nothing to pack there. One Makefile, both variants.
ifneq ($(wildcard filesystem/.),)
$(BUILD_DIR)/forge.dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
forge.z64: $(BUILD_DIR)/forge.dfs
endif

$(BUILD_DIR)/forge.elf: $(OBJS)

forge.z64: N64_ROM_TITLE = "Kiln Forge"

clean:
	rm -rf $(BUILD_DIR) forge.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
