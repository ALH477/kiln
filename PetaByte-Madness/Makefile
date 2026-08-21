# SPDX-License-Identifier: MPL-2.0
#
# PetaByte Madness — a standalone game flake output, alongside game/
# (Ganja Goblin) rather than under examples/.
#
# Include order: n64.mk, then t3d.mk, then kiln.mk. Each prepends its own -l
# to N64_LDFLAGS, so the last one included ends up leftmost on the link line
# — which is what we want, since libkiln calls into Tiny3D, which calls into
# libdragon.
BUILD_DIR = build
SOURCE_DIR = src
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

# Tiny3D's headers use C23-isms, so the dialect has to match.
N64_CFLAGS += -std=gnu2x -Wno-error

# Boot straight into one screen, for capture. `PM_JUMP=INTAKE` becomes
# -DPM_JUMP_SCREEN=PM_SCREEN_INTAKE, which pm_screens.c's boot_update reads.
#
# Appended to N64_CFLAGS here rather than passed as N64_CFLAGS+= on the make
# command line, for the reason nix/rom.nix's comment about KILN_DEBUG spells
# out: a command-line N64_CFLAGS assignment OVERRIDES the one n64.mk built and
# silently drops every include path with it.
#
# Only meaningful together with KILN_DEBUG — the jump lives inside pm_screens.c's
# debug block — so it is an error to ask for one without the other rather than
# a flag that quietly does nothing.
ifdef PM_JUMP
  ifndef KILN_DEBUG
    $(error PM_JUMP=$(PM_JUMP) needs KILN_DEBUG=1: the jump is part of the debug \
            build (pm_screens.c). Build the debug ROM variant instead.)
  endif
  N64_CFLAGS += -DPM_JUMP_SCREEN=PM_SCREEN_$(PM_JUMP)
endif

# Pin the veil fully on, for capture. See main.c's PM_VEIL_FORCE block: the
# palette swap only shows on a CI4-textured model, and the screens that have one
# are not the screens the veil runs on.
ifdef PM_VEIL_FORCE
  ifndef KILN_DEBUG
    $(error PM_VEIL_FORCE needs KILN_DEBUG=1.)
  endif
  N64_CFLAGS += -DPM_VEIL_FORCE=1
endif

# Which spatial debug-draw layer set is on at boot: 0 off, 1 cam, 2 clip,
# 3 actors, 4 all. See pm_debug.c's DD_SETS. Same KILN_DEBUG requirement and the
# same reason — a capture run cannot press the cycle chord.
# ── The cinematic debugger (pm_cine.h) ─────────────────────────────────
# Four flags, same KILN_DEBUG requirement and the same `$(error)` rather than a
# flag that quietly does nothing, for the reason PM_JUMP's comment gives.

# Arm the transport and draw the timeline at boot.
ifdef PM_CINE
  ifndef KILN_DEBUG
    $(error PM_CINE needs KILN_DEBUG=1: the transport is part of the debug \
            build (pm_cine.c).)
  endif
  N64_CFLAGS += -DPM_CINE=1
endif

# Seek to an exact SHOT time on the first frame, then hold. This is what makes
# a capture reproducible: `./dev shot <rom> out.png 6` settles for six seconds
# of WALL CLOCK, which depends on emulator speed and machine load, whereas
# PM_SHOT_AT=6.0 is the same picture on every machine and every run.
ifdef PM_SHOT_AT
  ifndef KILN_DEBUG
    $(error PM_SHOT_AT=$(PM_SHOT_AT) needs KILN_DEBUG=1.)
  endif
  N64_CFLAGS += -DPM_SHOT_AT=$(PM_SHOT_AT)f -DPM_CINE=1
endif

# Walk the shot in N evenly-spaced rungs, holding each for a fixed number of
# FRAMES — frames rather than seconds, so the content of a recording does not
# depend on how fast the emulator ran. One boot yields the whole contact sheet.
ifdef PM_SHOT_LADDER
  ifndef KILN_DEBUG
    $(error PM_SHOT_LADDER=$(PM_SHOT_LADDER) needs KILN_DEBUG=1.)
  endif
  N64_CFLAGS += -DPM_SHOT_LADDER=$(PM_SHOT_LADDER) -DPM_CINE=1
endif

# Boot straight into the camera-lint report over every registered shot, and
# nothing else. One capture reads the whole game's camera health.
ifdef PM_CINE_LINT
  ifndef KILN_DEBUG
    $(error PM_CINE_LINT needs KILN_DEBUG=1.)
  endif
  N64_CFLAGS += -DPM_CINE_LINT=1
endif

ifdef PM_DD
  ifndef KILN_DEBUG
    $(error PM_DD=$(PM_DD) needs KILN_DEBUG=1: the overlay is part of the debug \
            build (pm_debug.c).)
  endif
  N64_CFLAGS += -DPM_DD_INITIAL=$(PM_DD)
endif

# The camera validator is no longer built here. It moved into the engine as
# kiln_camlint when Forge's CAM mode became a second consumer, so it now arrives
# via -lkiln and PetaByte-Madness/src/pm_cine_lint.h is a shim over it. The
# reasoning that used to live here — that a source only ever built under
# KILN_DEBUG is one nobody notices breaking — is now satisfied more strongly:
# libkiln is built ONCE without KILN_DEBUG, so it compiles on every build of
# anything, and --gc-sections still drops it from a ROM that never calls it.
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
       $(BUILD_DIR)/pm_narration.o \
       $(BUILD_DIR)/pm_credits.o \
       $(BUILD_DIR)/pm_debug.o \
       $(BUILD_DIR)/pm_cine.o \

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
