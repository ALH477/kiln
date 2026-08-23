# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Builds libstreamdb_emb.a for the N64, installed into the shared $N64_INST
# prefix like libdragon/Tiny3D/libkiln so a ROM links it with -lstreamdb_emb.
BUILD_DIR = build
SOURCE_DIR = src
INSTALLDIR = $(N64_INST)

include $(N64_INST)/include/n64.mk

inc := include/streamdb_embedded.h

# The DFS backend is the on-console one; the stdio backend is host-only and
# deliberately not built here (there is no stdio filesystem on a ROM).
N64_CFLAGS += -std=gnu11 -Os -Iinclude -Wall -Wextra -Wno-error \
              -DSTREAMDB_EMB_BACKEND_DFS=1

OBJ = $(BUILD_DIR)/streamdb_embedded.o $(BUILD_DIR)/streamdb_io_dfs.o

all: $(BUILD_DIR)/libstreamdb_emb.a

$(BUILD_DIR)/libstreamdb_emb.a: $(OBJ)
	@mkdir -p $(dir $@)
	@echo "    [AR] $@"
	$(N64_AR) rcs $@ $^

$(BUILD_DIR)/%.o: $(SOURCE_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(N64_CC) -c $(N64_CFLAGS) -o $@ $<

install: all
	mkdir -p $(INSTALLDIR)/$(N64_TARGET)/include/streamdb
	install -cv -m 0644 streamdb-inst.mk $(INSTALLDIR)/include/streamdb.mk
	install -Cv -m 0644 $(inc) $(INSTALLDIR)/$(N64_TARGET)/include/streamdb/
	install -Cv -m 0644 $(BUILD_DIR)/libstreamdb_emb.a $(INSTALLDIR)/$(N64_TARGET)/lib

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all install clean
