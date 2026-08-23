# SPDX-License-Identifier: MIT
#
# The Kiln engine: a 3D layer on Tiny3D plus a 2D GUI layer on rdpq.
#
# Shaped like Tiny3D's own Makefile so it installs into the same $N64_INST
# layout and can be merged into one prefix (see nix/n64-inst.nix).

BUILD_DIR = build
SOURCE_DIR = src/kiln
INSTALLDIR = $(N64_INST)

include $(N64_INST)/include/n64.mk

# The module list lives in its own file so the native/host build can include
# the same one — see engine/modules.mk's header for why that matters.
include modules.mk

src := $(addprefix $(SOURCE_DIR)/,$(addsuffix .c,$(MODULES)))
inc := $(addprefix $(SOURCE_DIR)/,$(addsuffix .h,$(MODULES))) \
       $(addprefix $(SOURCE_DIR)/,$(addsuffix .h,$(HEADER_ONLY)))

# gnu2x to match Tiny3D's own dialect (its headers use C23-isms).
# -lstreamdb_emb is pulled in by a ROM's Makefile via streamdb.mk; the engine
# archive only references streamdb_emb_* symbols, which resolve at the ROM's
# final link. A ROM that doesn't use kiln_asset pays nothing for it —
# --gc-sections drops the unused object.
N64_CFLAGS += -std=gnu2x -Os -Isrc -Wall -Wextra -Wno-error \
              -DSTREAMDB_EMB_BACKEND_DFS=1

OBJ = $(addprefix $(BUILD_DIR)/,$(addsuffix .o,$(MODULES)))

all: $(BUILD_DIR)/libkiln.a

$(BUILD_DIR)/libkiln.a: $(OBJ)
	@mkdir -p $(dir $@)
	@echo "    [AR] $@"
	$(N64_AR) rcs $@ $^

$(BUILD_DIR)/%.o: $(SOURCE_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(N64_CC) -c $(N64_CFLAGS) -o $@ $<

install: all
	mkdir -p $(INSTALLDIR)/$(N64_TARGET)/include/kiln
	install -cv -m 0644 kiln-inst.mk $(INSTALLDIR)/include/kiln.mk
	for file in $(inc); do \
		install -Cv -m 0644 $$file $(INSTALLDIR)/$(N64_TARGET)/include/kiln; \
	done
	install -Cv -m 0644 $(BUILD_DIR)/libkiln.a $(INSTALLDIR)/$(N64_TARGET)/lib

clean:
	rm -rf $(BUILD_DIR)

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all install clean
