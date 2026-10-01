# SPDX-License-Identifier: MIT
#
# The Exsecutor logo, rendered by Kiln's built-in software 3D path
# (fig_soft3d) from Exsecutor-emitted C. See main.c for what the screen
# shows and ../../engine/src/kiln/gen/PROVENANCE.md for where the generated
# unit and the EXSG stream come from.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/streamdb.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

# fig_soft3d is registered in the engine (modules.mk), but the flake's
# n64-inst prefix is NOT rewired to rebuild libfigulina with it yet (see
# exsec-signaculum-anim.nix's DEFERRED note) — so the installed
# <kiln/kiln_soft3d.h> does not exist and the installed libfigulina.a has no
# such member. Until then this ROM compiles against the ENGINE SOURCE TREE:
# the header resolves via the first -I below, the module's object is built
# by ../../engine's own Makefile (which is where its -fno-fast-math
# exception lives) and linked ahead of the archive's stale -lkiln.
#
#   make -C ../../engine        # once, whenever engine sources move
# (`all` is named: engine/Makefile includes modules.mk before its `all:`,
# so the default goal there is print-modules, not the archive.)
ENGINE_DIR := $(abspath ../../engine)
N64_CFLAGS += -I$(ENGINE_DIR)/src -I$(ENGINE_DIR)/src/kiln
SOFT3D_OBJ := $(ENGINE_DIR)/build/kiln_soft3d.o

OBJS = $(BUILD_DIR)/main.o

$(SOFT3D_OBJ):
	@echo "==> building the engine in-tree for $(@F)"
	$(MAKE) -C $(ENGINE_DIR) all

# THE GENERATED UNIT REFUSES -ffast-math, which n64.mk turns on for
# everything. Its prologue has `#error "exsecutor: -ffast-math is refused
# (spec 5.4)"`, and that refusal is correct — a compiler told it may assume
# no NaNs and reorder floating point is not compiling the program Exsecutor
# defines. The object above gets -fno-fast-math from engine/Makefile
# (appended after n64.mk's flag so it wins); the prototype guard below gets
# the same treatment here. Do not "fix" a build failure by deleting the
# #error. `all` stays the pinned goal, before the guard's explicit recipe.
.DEFAULT_GOAL := all

GEN_CFLAGS = -Werror -fno-fast-math -Wno-unused-function

# NO PROTOTYPE GUARD TU HERE, on purpose. exsec-signaculum-demo compiles
# exsec_proto_check.c on every build and that pair is the check; a second copy
# in this directory would be a second place to keep in sync, not a second
# check. This ROM links the same engine object and so inherits the same
# header-versus-generated-unit agreement.
all: exsec-signaculum-anim.z64

$(BUILD_DIR)/exsec-signaculum-anim.dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
$(BUILD_DIR)/exsec-signaculum-anim.elf: $(OBJS) $(SOFT3D_OBJ)

exsec-signaculum-anim.z64: N64_ROM_TITLE = "Kiln Signaculum Anim"
exsec-signaculum-anim.z64: $(BUILD_DIR)/exsec-signaculum-anim.dfs

clean:
	rm -rf $(BUILD_DIR) exsec-signaculum-anim.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
