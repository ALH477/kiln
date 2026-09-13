# SPDX-License-Identifier: MIT
#
# Every example on one cartridge. main.c is the menu; demos.def is the list,
# which this file reads rather than repeats.
#
# THE SOURCES ARE THE EXAMPLES' OWN, UNCHANGED, read from ../<dir>/ -- which is
# why flake.nix builds this ROM with the whole examples/ tree as src and this
# directory as sourceRoot. Each is compiled with -Dmain=demo_<id>_main.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
# streamdb.mk BEFORE kiln.mk, for the link-order reason examples/streamdb-demo
# gives: -lkiln -lstreamdb_emb -lt3d -ldragon.
include $(N64_INST)/include/streamdb.mk
include $(N64_INST)/include/kiln.mk

# Pinned, because the per-object rules below are defined before `all:`.
.DEFAULT_GOAL := all

FAUST_VOICE ?= $(error FAUST_VOICE must point to the ks-voice .o file (live-voice links it))

# ── The list ──────────────────────────────────────────────────────────
# id:dir:src per row. A row that does not parse would silently leave its demo
# out of the link while main.c still names it -- a link error, but a confusing
# one -- so the row count is compared with the DEMO( line count.
# Both patterns are variables because make counts parentheses inside $(shell)
# and the `\(` of a regex leaves one open ("unterminated call to function").
DEMO_SED   := s/^DEMO\(([a-z0-9_]+), *"([a-z0-9-]+)", *"([a-z0-9_.]+)".*/\1:\2:\3/p
DEMO_START := ^DEMO(
DEMO_ROWS  := $(shell sed -nE '$(DEMO_SED)' demos.def)
DEMO_IDS   := $(foreach r,$(DEMO_ROWS),$(word 1,$(subst :, ,$(r))))
DEMO_COUNT := $(shell grep -c '$(DEMO_START)' demos.def)
ifneq ($(words $(DEMO_ROWS)),$(DEMO_COUNT))
$(error demos.def: $(DEMO_COUNT) DEMO rows but $(words $(DEMO_ROWS)) parsed -- see its header for the shape)
endif

# ── Each demo's flags, as its own Makefile had them ───────────────────
# Every Kiln-era example adds `-std=gnu2x -Wno-error`. The four older ones
# (hello, audio, music, live-voice) add nothing, so they build at libdragon's
# -std=gnu17 with -Werror, as they always have. debug-demo is the ROM flake.nix
# builds with debugConsole = true; KILN_DEBUG is per object here, because
# oot-demo is in the same link and its standalone ROM is built without it.
MODERN := -std=gnu2x -Wno-error
DEMO_CFLAGS_hello      :=
DEMO_CFLAGS_audio      :=
DEMO_CFLAGS_music      :=
DEMO_CFLAGS_live_voice :=
DEMO_CFLAGS_debug      := $(MODERN) -DKILN_DEBUG=1
demo_cflags = $(if $(filter undefined,$(origin DEMO_CFLAGS_$(1))),$(MODERN),$(DEMO_CFLAGS_$(1)))

# Explicit recipes naming $(N64_CC) and $(N64_CFLAGS), not n64.mk's $(CC) and
# $(CFLAGS): those are the N64 toolchain only as target-specific variables on
# %.z64, and examples/exsec-streamdb-demo/Makefile records what happened when
# an object missed that inheritance (the host gcc compiled it).
define demo_object
$(BUILD_DIR)/demos/$(1).o: ../$(2)/$(3)
	@mkdir -p $$(dir $$@)
	@echo "    [CC] $$< as demo_$(1)_main"
	$$(N64_CC) -c $$(N64_CFLAGS) $$(call demo_cflags,$(1)) -Dmain=demo_$(1)_main -MMD -o $$@ $$<
endef
$(foreach r,$(DEMO_ROWS),$(eval $(call demo_object,$(word 1,$(subst :, ,$(r))),$(word 2,$(subst :, ,$(r))),$(word 3,$(subst :, ,$(r))))))

# The Exsecutor demo's second object, with the flags its Makefile explains:
# the generated unit refuses -ffast-math, and is kept warning-clean.
$(BUILD_DIR)/demos/lector_streamdb.gen.o: ../exsec-streamdb-demo/lector_streamdb.gen.c
	@mkdir -p $(dir $@)
	@echo "    [CC] $<"
	$(N64_CC) -c $(N64_CFLAGS) $(MODERN) -Werror -fno-fast-math -Wno-unused-function -MMD -o $@ $<

# ── The menu ─────────────────────────────────────────────────────────
$(BUILD_DIR)/main.o: demos.def
ifdef AUTORUN
$(if $(filter $(AUTORUN),$(DEMO_IDS)),,$(error AUTORUN=$(AUTORUN) is not an id in demos.def))
$(BUILD_DIR)/main.o: N64_CFLAGS += -DREEL_AUTORUN=\"$(AUTORUN)\"
endif

OBJS = $(BUILD_DIR)/main.o \
       $(DEMO_IDS:%=$(BUILD_DIR)/demos/%.o) \
       $(BUILD_DIR)/demos/lector_streamdb.gen.o \
       $(FAUST_VOICE)

all: demo-reel.z64

$(BUILD_DIR)/demo-reel.dfs: $(wildcard filesystem/*) $(wildcard filesystem/*/*)
$(BUILD_DIR)/demo-reel.elf: $(OBJS)

demo-reel.z64: N64_ROM_TITLE = "Kiln Demo Reel"
demo-reel.z64: $(BUILD_DIR)/demo-reel.dfs

clean:
	rm -rf $(BUILD_DIR) demo-reel.z64

-include $(wildcard $(BUILD_DIR)/*.d $(BUILD_DIR)/demos/*.d)

.PHONY: all clean
