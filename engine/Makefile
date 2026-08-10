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

src := $(SOURCE_DIR)/m64_engine.c $(SOURCE_DIR)/m64_gui.c $(SOURCE_DIR)/m64_actor.c $(SOURCE_DIR)/m64_room.c $(SOURCE_DIR)/m64_asset.c $(SOURCE_DIR)/m64_audio.c $(SOURCE_DIR)/m64_camera.c $(SOURCE_DIR)/m64_skel.c $(SOURCE_DIR)/m64_input.c $(SOURCE_DIR)/m64_clip.c $(SOURCE_DIR)/m64_dict.c $(SOURCE_DIR)/m64_map.c $(SOURCE_DIR)/m64_surface.c $(SOURCE_DIR)/m64_sound.c $(SOURCE_DIR)/m64_event.c $(SOURCE_DIR)/m64_target.c $(SOURCE_DIR)/m64_player.c $(SOURCE_DIR)/m64_physics.c $(SOURCE_DIR)/m64_texanim.c $(SOURCE_DIR)/m64_vanim.c $(SOURCE_DIR)/m64_fpscam.c $(SOURCE_DIR)/m64_weapon.c $(SOURCE_DIR)/m64_inventory.c $(SOURCE_DIR)/m64_projectile.c $(SOURCE_DIR)/m64_trigger.c $(SOURCE_DIR)/m64_context.c $(SOURCE_DIR)/m64_dialogue.c $(SOURCE_DIR)/m64_weapons.c $(SOURCE_DIR)/m64_scratch.c $(SOURCE_DIR)/m64_cache.c $(SOURCE_DIR)/m64_tile.c $(SOURCE_DIR)/m64_lod.c $(SOURCE_DIR)/m64_twopass.c $(SOURCE_DIR)/m64_console.c $(SOURCE_DIR)/m64_panic.c $(SOURCE_DIR)/m64_prof.c $(SOURCE_DIR)/m64_rng.c $(SOURCE_DIR)/m64_dice.c $(SOURCE_DIR)/m64_board.c $(SOURCE_DIR)/m64_turn.c $(SOURCE_DIR)/m64_char.c $(SOURCE_DIR)/m64_widget.c $(SOURCE_DIR)/m64_save.c $(SOURCE_DIR)/m64_splash.c
inc := $(SOURCE_DIR)/m64_engine.h $(SOURCE_DIR)/m64_gui.h $(SOURCE_DIR)/m64_actor.h $(SOURCE_DIR)/m64_room.h $(SOURCE_DIR)/m64_asset.h $(SOURCE_DIR)/m64_audio.h $(SOURCE_DIR)/m64_camera.h $(SOURCE_DIR)/m64_skel.h $(SOURCE_DIR)/m64_input.h $(SOURCE_DIR)/m64_clip.h $(SOURCE_DIR)/m64_dict.h $(SOURCE_DIR)/m64_map.h $(SOURCE_DIR)/m64_surface.h $(SOURCE_DIR)/m64_sound.h $(SOURCE_DIR)/m64_event.h $(SOURCE_DIR)/m64_target.h $(SOURCE_DIR)/m64_player.h $(SOURCE_DIR)/m64_physics.h $(SOURCE_DIR)/m64_texanim.h $(SOURCE_DIR)/m64_vanim.h $(SOURCE_DIR)/m64_fpscam.h $(SOURCE_DIR)/m64_weapon.h $(SOURCE_DIR)/m64_inventory.h $(SOURCE_DIR)/m64_projectile.h $(SOURCE_DIR)/m64_trigger.h $(SOURCE_DIR)/m64_context.h $(SOURCE_DIR)/m64_dialogue.h $(SOURCE_DIR)/m64_weapons.h $(SOURCE_DIR)/m64_scratch.h $(SOURCE_DIR)/m64_cache.h $(SOURCE_DIR)/m64_tile.h $(SOURCE_DIR)/m64_lod.h $(SOURCE_DIR)/m64_twopass.h $(SOURCE_DIR)/m64_console.h $(SOURCE_DIR)/m64_panic.h $(SOURCE_DIR)/m64_prof.h $(SOURCE_DIR)/m64_rng.h $(SOURCE_DIR)/m64_dice.h $(SOURCE_DIR)/m64_board.h $(SOURCE_DIR)/m64_turn.h $(SOURCE_DIR)/m64_char.h $(SOURCE_DIR)/m64_widget.h $(SOURCE_DIR)/m64_save.h $(SOURCE_DIR)/m64_splash.h

# gnu2x to match Tiny3D's own dialect (its headers use C23-isms).
# -lstreamdb_emb is pulled in by a ROM's Makefile via streamdb.mk; the engine
# archive only references streamdb_emb_* symbols, which resolve at the ROM's
# final link. A ROM that doesn't use m64_asset pays nothing for it —
# --gc-sections drops the unused object.
N64_CFLAGS += -std=gnu2x -Os -Isrc -Wall -Wextra -Wno-error \
              -DSTREAMDB_EMB_BACKEND_DFS=1

OBJ = $(BUILD_DIR)/m64_engine.o $(BUILD_DIR)/m64_gui.o $(BUILD_DIR)/m64_actor.o $(BUILD_DIR)/m64_room.o $(BUILD_DIR)/m64_asset.o $(BUILD_DIR)/m64_audio.o $(BUILD_DIR)/m64_camera.o $(BUILD_DIR)/m64_skel.o $(BUILD_DIR)/m64_input.o $(BUILD_DIR)/m64_clip.o $(BUILD_DIR)/m64_dict.o $(BUILD_DIR)/m64_map.o $(BUILD_DIR)/m64_surface.o $(BUILD_DIR)/m64_sound.o $(BUILD_DIR)/m64_event.o $(BUILD_DIR)/m64_target.o $(BUILD_DIR)/m64_player.o $(BUILD_DIR)/m64_physics.o $(BUILD_DIR)/m64_texanim.o $(BUILD_DIR)/m64_vanim.o $(BUILD_DIR)/m64_fpscam.o $(BUILD_DIR)/m64_weapon.o $(BUILD_DIR)/m64_inventory.o $(BUILD_DIR)/m64_projectile.o $(BUILD_DIR)/m64_trigger.o $(BUILD_DIR)/m64_context.o $(BUILD_DIR)/m64_dialogue.o $(BUILD_DIR)/m64_weapons.o $(BUILD_DIR)/m64_scratch.o $(BUILD_DIR)/m64_cache.o $(BUILD_DIR)/m64_tile.o $(BUILD_DIR)/m64_lod.o $(BUILD_DIR)/m64_twopass.o $(BUILD_DIR)/m64_console.o $(BUILD_DIR)/m64_panic.o $(BUILD_DIR)/m64_prof.o $(BUILD_DIR)/m64_rng.o $(BUILD_DIR)/m64_dice.o $(BUILD_DIR)/m64_board.o $(BUILD_DIR)/m64_turn.o $(BUILD_DIR)/m64_char.o $(BUILD_DIR)/m64_widget.o $(BUILD_DIR)/m64_save.o $(BUILD_DIR)/m64_splash.o

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
