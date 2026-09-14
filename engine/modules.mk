# SPDX-License-Identifier: MIT
#
# engine/modules.mk — the module list, and the only place it exists.
#
# ONE list. `src`, `inc` and `OBJ` used to be three hand-maintained lists of
# the same 46 names written out in full, which meant adding a module was a
# four-place edit (three here, plus nix/engine.nix's installCheck) and
# forgetting one place failed in a different way each time: a missed `src`
# entry links with undefined symbols at the ROM, a missed `inc` entry installs
# a library whose header is absent, and a missed installCheck entry fails
# silently forever. nix/engine.nix reads these variables rather than restating
# them, via `make print-modules` / `print-headers`.
#
# It lives in its own file rather than in engine/Makefile because the Makefile
# cannot be the home of it any more: a second build of the same sources — the
# native/host one — has to compile exactly this list or the two targets drift
# apart module by module, which is the thing the list existed to prevent in
# the first place. An `include` is how that stays true without a copy.
#
# Order is load-bearing to the extent that `ar rcs` writes members in argument
# order — keep appends at the end so an unrelated bump does not rewrite every
# byte of libkiln.a.
MODULES := \
	kiln_engine kiln_gui kiln_actor kiln_room kiln_asset kiln_audio kiln_camera \
	kiln_skel kiln_input kiln_clip kiln_dict kiln_map kiln_surface kiln_sound \
	kiln_event kiln_target kiln_player kiln_physics kiln_texanim kiln_vanim \
	kiln_fpscam kiln_weapon kiln_inventory kiln_projectile kiln_trigger \
	kiln_context kiln_dialogue kiln_weapons kiln_scratch kiln_cache kiln_tile \
	kiln_lod kiln_twopass kiln_console kiln_panic kiln_prof kiln_rng kiln_dice \
	kiln_board kiln_turn kiln_char kiln_widget kiln_save kiln_splash kiln_crater \
	kiln_video kiln_debugdraw kiln_store kiln_voxel kiln_voxmesh kiln_camlint \
	kiln_stream kiln_streamio kiln_prim kiln_pose \
	kiln_soft3d

# ── Header-only modules ────────────────────────────────────────────────
# Installed, but contributing no object. There is exactly one so far and it is
# header-only for a reason its own file states at length: kiln_camkey is one
# small inlined function that the runtime, the debug overlay and a NATIVE
# validator all have to share, and giving it a .c would mean adding it to every
# consumer's OBJS and to two check compile lines — "two more places to forget".
#
# A separate list rather than a member of MODULES because MODULES drives
# $(OBJS), and a name in there with no .c fails the archive with "No rule to
# make target". nix/engine.nix asks for both lists, so a header-only module is
# still checked for being installed.
# kiln_levelvocab is generated (tools/schema/level_vocab.py) and contributes no
# object, same as kiln_camkey — it is macros only.
HEADER_ONLY := kiln_camkey kiln_levelvocab

# ── The host tier ──────────────────────────────────────────────────────
# The modules that compile NATIVELY, against plat/host/include's <libdragon.h>
# system surface plus nix/host-math.nix's build of libdragon's own fast math.
# Same sources, no #ifdef, no fork — see plat/host/include/libdragon.h.
#
# This list is a CLAIM, and nix/checks/kiln-parity.nix checks it in both
# directions: every name here must compile, and every name absent from it must
# NOT. So it cannot drift either way — adding a module that does not build
# fails, and making a module host-clean without listing it fails too.
#
# The list grows as plat/host/include's surface does, and kiln-parity is what
# makes that visible: widening <libdragon.h> to cover the 2D pass immediately
# failed the gate with "kiln_gui compiles natively but is NOT in
# HOST_MODULES", which is the notification you want rather than a module
# quietly missing out on -Werror for a year.
#
# What keeps the rest out is the compiler, not this comment. As of the 2D
# tier: 25 want <t3d/t3d.h>, 5 want <t3d/t3dmodel.h>, 1 <libcart/cart.h>,
# 1 <video.h>, and 3 want a specific libdragon type not yet shimmed
# (JOYPAD_PORT_COUNT, eepfs_entry_t, exception_handler_t). Most of the 25 are
# bound only TRANSITIVELY, through a header that pulls in t3d.h rather than
# through anything they do themselves.
#
# Compiling here is not the same as linking here: kiln_dialogue, kiln_prof,
# kiln_sound, kiln_surface and kiln_widget call into tiers that are not built
# yet, so they get the -Werror second opinion but cannot be run standalone.
# kiln_gui both compiles AND links, and nix/checks/kiln-gui.nix renders a real
# HUD through it. nix/checks/kiln-logic.nix keeps its own list of what it
# ASSERTS on, with the reasoning at the point of use.
HOST_MODULES := \
	kiln_actor kiln_asset kiln_audio kiln_board kiln_cache kiln_camera \
	kiln_camlint kiln_char kiln_clip kiln_console kiln_context kiln_crater \
	kiln_debugdraw kiln_dialogue kiln_dice kiln_dict kiln_engine kiln_event \
	kiln_fpscam kiln_gui kiln_input kiln_inventory kiln_lod kiln_map \
	kiln_physics kiln_player kiln_prof kiln_projectile kiln_rng kiln_room \
	kiln_save kiln_scratch kiln_skel kiln_sound kiln_splash kiln_store \
	kiln_stream kiln_streamio kiln_surface kiln_target kiln_texanim kiln_tile \
	kiln_trigger kiln_turn kiln_twopass kiln_vanim kiln_voxel kiln_voxmesh \
	kiln_weapon kiln_weapons \
	kiln_widget kiln_prim kiln_pose \
	kiln_soft3d

# nix/engine.nix's installCheck and nix/checks/kiln-parity.nix ask make for
# these rather than restating any list in Nix — which is exactly how the old
# arrangement came to be verifying 36 of 46 headers.
print-modules:
	@echo $(MODULES)

# Every name that must have an installed header — MODULES plus the header-only
# ones. nix/engine.nix diffs this against what actually landed in the prefix.
print-headers:
	@echo $(MODULES) $(HEADER_ONLY)

print-host-modules:
	@echo $(HOST_MODULES)

.PHONY: print-modules print-headers print-host-modules
