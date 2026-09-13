# SPDX-License-Identifier: MIT
#
# A StreamDB reader written in Exsecutor, on the N64. See main.c for what the
# screen shows and PROVENANCE.md for where lector_streamdb.gen.c came from.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
# streamdb.mk BEFORE kiln.mk, for the link-order reason examples/streamdb-demo
# gives: -lkiln -lstreamdb_emb -lt3d -ldragon.
include $(N64_INST)/include/streamdb.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o $(BUILD_DIR)/lector_streamdb.gen.o

# THE GENERATED UNIT REFUSES -ffast-math, which n64.mk turns on for everything.
# Its prologue has `#error "exsecutor: -ffast-math is refused (spec 5.4)"`, and
# that refusal is correct -- a compiler told it may assume no NaNs and reorder
# floating point is not compiling the program Exsecutor defines. So this one
# object gets -fno-fast-math, placed after n64.mk's flag so it wins. Do not
# "fix" a build failure here by deleting the #error.
#
# -Werror is put back for it (the line above relaxes it for main.c) because the
# generated C is warning-clean under -Wall and should stay provably so.
# -Wno-unused-function because its helper blob is a fixed prologue, and a unit
# that does no signed arithmetic still carries the signed helpers.
# `all` is the goal, pinned. Without this line, the first target in the file is
# make's default -- and the prototype guard's explicit recipe below comes
# before `all:`, so a bare `make` built the guard alone and stopped, and
# mkN64Rom (rightly) failed the build for producing no .dfs.
.DEFAULT_GOAL := all

GEN_CFLAGS = -Werror -fno-fast-math -Wno-unused-function
$(BUILD_DIR)/lector_streamdb.gen.o: N64_CFLAGS += $(GEN_CFLAGS)

# The prototype guard is compiled on every build and deliberately NOT linked:
# see exsec_proto_check.c.
#
# IT HAS ITS OWN RECIPE, NAMING $(N64_CC), AND THAT IS NOT OPTIONAL. n64.mk's
# `$(BUILD_DIR)/%.o: $(SOURCE_DIR)/%.c` rule compiles with plain `$(CC)` and
# `$(CFLAGS)`, which n64.mk points at the N64 toolchain only as TARGET-SPECIFIC
# variables on `%.z64` (n64.mk lines 125-131: `%.z64: CC=$(N64_CC)`,
# `%.z64: CFLAGS+=$(N64_CFLAGS)`) -- and make hands those only down that
# target's own prerequisite chain, .z64 -> .elf -> objects. An object that is a
# prerequisite of `all` and not of the ROM gets neither, and is compiled by the
# HOST's x86-64 gcc. That is
# exactly what happened on the first build: this guard was built for x86-64,
# and the two `_Static_assert(sizeof(void *) == 4)`s -- this directory's and the
# generated unit's -- refused it. The asserts were right; the compiler was wrong.
$(BUILD_DIR)/exsec_proto_check.o: exsec_proto_check.c exsec_streamdb.h lector_streamdb.gen.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $< (prototype guard, not linked)"
	$(N64_CC) -c $(N64_CFLAGS) $(GEN_CFLAGS) -o $@ $<

all: $(BUILD_DIR)/exsec_proto_check.o exsec-streamdb-demo.z64

$(BUILD_DIR)/exsec-streamdb-demo.dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
$(BUILD_DIR)/exsec-streamdb-demo.elf: $(OBJS)

exsec-streamdb-demo.z64: N64_ROM_TITLE = "Kiln Exsecutor"
exsec-streamdb-demo.z64: $(BUILD_DIR)/exsec-streamdb-demo.dfs

clean:
	rm -rf $(BUILD_DIR) exsec-streamdb-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
