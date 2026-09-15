# SPDX-License-Identifier: MIT
#
# The Exsecutor logo, rendered by Kiln's built-in software 3D path
# (kiln_soft3d) from Exsecutor-emitted C. See main.c for what the screen
# shows and ../../engine/src/kiln/gen/PROVENANCE.md for where the generated
# unit and the EXSG stream come from.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/streamdb.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

# kiln_soft3d is registered in the engine (modules.mk), but the flake's
# n64-inst prefix is NOT rewired to rebuild libkiln with it yet (see
# exsec-signaculum-demo.nix's DEFERRED note) — so the installed
# <kiln/kiln_soft3d.h> does not exist and the installed libkiln.a has no
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

# The prototype guard is compiled on every build and deliberately NOT
# linked: see exsec_proto_check.c. IT HAS ITS OWN RECIPE, NAMING $(N64_CC)
# for the same reason exsec-streamdb-demo's does — the default %.o rule
# compiles with the host $(CC), and the unit's 32-bit-pointer _Static_assert
# is right to refuse that compiler.
$(BUILD_DIR)/exsec_proto_check.o: exsec_proto_check.c ../../engine/src/kiln/kiln_soft3d.h ../../engine/src/kiln/gen/signaculum_mips64.gen.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $< (prototype guard, not linked)"
	$(N64_CC) -c $(N64_CFLAGS) $(GEN_CFLAGS) -o $@ $<

all: $(BUILD_DIR)/exsec_proto_check.o exsec-signaculum-demo.z64

$(BUILD_DIR)/exsec-signaculum-demo.dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
$(BUILD_DIR)/exsec-signaculum-demo.elf: $(OBJS) $(SOFT3D_OBJ)

exsec-signaculum-demo.z64: N64_ROM_TITLE = "Kiln Exsecutor Signaculum"
exsec-signaculum-demo.z64: $(BUILD_DIR)/exsec-signaculum-demo.dfs

clean:
	rm -rf $(BUILD_DIR) exsec-signaculum-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
