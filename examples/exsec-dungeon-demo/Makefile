# SPDX-License-Identifier: MIT
#
# A dungeon grown from one 64-bit seed by code written in Exsecutor, on the N64.
# See main.c for what the screen shows and PROVENANCE.md for where
# dungeon_mips64.gen.c came from.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
# streamdb.mk BEFORE kiln.mk, for the link-order reason examples/streamdb-demo
# gives: -lkiln -lstreamdb_emb -lt3d -ldragon.
include $(N64_INST)/include/streamdb.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o $(BUILD_DIR)/dungeon_view.o $(BUILD_DIR)/dungeon_mips64.gen.o

# THE GENERATED UNIT REFUSES -ffast-math, which n64.mk turns on for everything.
# Its prologue has `#error "exsecutor: -ffast-math is refused (spec 5.4)"`, and
# that refusal is correct -- a compiler told it may assume no NaNs and reorder
# floating point is not compiling the program Exsecutor defines. So this one
# object gets -fno-fast-math, placed after n64.mk's flag so it wins. Do not
# "fix" a build failure here by deleting the #error. (This unit has no float
# arithmetic at all, so the refusal costs nothing; it is the unit's blanket one.)
#
# -Werror is put back for it (the line above relaxes it for main.c) because the
# generated C is warning-clean under -Wall and should stay provably so.
# -Wno-unused-function because its helper blob is a fixed prologue, and a unit
# that does no signed arithmetic still carries the signed helpers.
#
# `all` is the goal, pinned, for the reason examples/exsec-streamdb-demo's
# Makefile gives: the prototype guard's explicit recipe below comes before
# `all:`, so without this a bare `make` built the guard alone and stopped.
.DEFAULT_GOAL := all

GEN_CFLAGS = -Werror -fno-fast-math -Wno-unused-function
$(BUILD_DIR)/dungeon_mips64.gen.o: N64_CFLAGS += $(GEN_CFLAGS)

# The prototype guard has its own recipe naming $(N64_CC): n64.mk's generic
# %.o rule compiles with the HOST compiler for an object that is not a
# prerequisite of the .z64, and the generated unit's 32-bit-pointer
# _Static_assert is right to refuse that. exsec-streamdb-demo's Makefile has the
# full account.
$(BUILD_DIR)/exsec_proto_check.o: exsec_proto_check.c exsec_dungeon.h dungeon_view.h dungeon_golden.h dungeon_mips64.gen.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $< (prototype guard, not linked)"
	$(N64_CC) -c $(N64_CFLAGS) $(GEN_CFLAGS) -o $@ $<

all: $(BUILD_DIR)/exsec_proto_check.o exsec-dungeon-demo.z64

$(BUILD_DIR)/exsec-dungeon-demo.elf: $(OBJS)

exsec-dungeon-demo.z64: N64_ROM_TITLE = "Kiln Exsec Dungeon"

clean:
	rm -rf $(BUILD_DIR) exsec-dungeon-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
