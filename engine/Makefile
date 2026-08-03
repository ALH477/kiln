# SPDX-License-Identifier: MPL-2.0
#
# The M64 engine: a 3D layer on Tiny3D plus a 2D GUI layer on rdpq.
#
# Shaped like Tiny3D's own Makefile so it installs into the same $N64_INST
# layout and can be merged into one prefix (see nix/n64-inst.nix).

BUILD_DIR = build
SOURCE_DIR = src/m64
INSTALLDIR = $(N64_INST)

include $(N64_INST)/include/n64.mk

src := $(SOURCE_DIR)/m64_engine.c $(SOURCE_DIR)/m64_gui.c $(SOURCE_DIR)/m64_actor.c $(SOURCE_DIR)/m64_room.c $(SOURCE_DIR)/m64_asset.c
inc := $(SOURCE_DIR)/m64_engine.h $(SOURCE_DIR)/m64_gui.h $(SOURCE_DIR)/m64_actor.h $(SOURCE_DIR)/m64_room.h $(SOURCE_DIR)/m64_asset.h

# gnu2x to match Tiny3D's own dialect (its headers use C23-isms).
# -lstreamdb_emb is pulled in by a ROM's Makefile via streamdb.mk; the engine
# archive only references streamdb_emb_* symbols, which resolve at the ROM's
# final link. A ROM that doesn't use m64_asset pays nothing for it —
# --gc-sections drops the unused object.
N64_CFLAGS += -std=gnu2x -Os -Isrc -Wall -Wextra -Wno-error

OBJ = $(BUILD_DIR)/m64_engine.o $(BUILD_DIR)/m64_gui.o $(BUILD_DIR)/m64_actor.o $(BUILD_DIR)/m64_room.o $(BUILD_DIR)/m64_asset.o

all: $(BUILD_DIR)/libm64.a

$(BUILD_DIR)/libm64.a: $(OBJ)
	@mkdir -p $(dir $@)
	@echo "    [AR] $@"
	$(N64_AR) rcs $@ $^

$(BUILD_DIR)/%.o: $(SOURCE_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(N64_CC) -c $(N64_CFLAGS) -o $@ $<

install: all
	mkdir -p $(INSTALLDIR)/$(N64_TARGET)/include/m64
	install -cv -m 0644 m64-inst.mk $(INSTALLDIR)/include/m64.mk
	for file in $(inc); do \
		install -Cv -m 0644 $$file $(INSTALLDIR)/$(N64_TARGET)/include/m64; \
	done
	install -Cv -m 0644 $(BUILD_DIR)/libm64.a $(INSTALLDIR)/$(N64_TARGET)/lib

clean:
	rm -rf $(BUILD_DIR)

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all install clean
