# SPDX-License-Identifier: MIT
#
# Included by a ROM's Makefile after n64.mk and t3d.mk. Order matters at link
# time: libfigulina calls into Tiny3D, so -lfigulina has to come before -lt3d,
# which t3d.mk has already prepended to N64_LDFLAGS.
#
# libkiln.a still exists beside it as a symlink for one migration train
# (docs/NAMING.md section 9 step 3), so a downstream that pins -lkiln itself
# keeps linking. Nothing in this tree does.
N64_LDFLAGS := -lfigulina $(N64_LDFLAGS)

# Translate the KILN_DEBUG make variable (set by mkN64Rom when debugConsole=true)
# into the actual compiler define. Doing this here, after n64.mk has already
# established N64_CFLAGS, avoids the precedence trap of passing
# N64_CFLAGS+=-DKILN_DEBUG=1 on the make command line, which would override
# n64.mk's default and drop the include paths.
ifdef KILN_DEBUG
  N64_CFLAGS += -DKILN_DEBUG=1
endif

# ── Jump ROMs ──────────────────────────────────────────────────────────
# KILN_JUMP=CORNER (set by flake.nix's mkJumpRoms) becomes
# -DKILN_JUMP=JUMP_CORNER: a build of an example that boots straight into one
# state, so `./dev shot` can capture it with no controller — the Forge mode ROMs'
# pattern (FORGE_MODE=CAM), generalised. The example owns the enum:
#
#     enum { JUMP_NONE, JUMP_CORNER };
#     #ifndef KILN_JUMP
#     #define KILN_JUMP JUMP_NONE
#     #endif
#
# libkiln.a is still built once, without it; only a ROM's own sources read it,
# the same rule KILN_DEBUG follows and for the same reason.
ifdef KILN_JUMP
  N64_CFLAGS += -DKILN_JUMP=JUMP_$(KILN_JUMP)
endif

# ── The prefix migration train ─────────────────────────────────────────
# docs/NAMING.md section 9 step 2. The engine's symbols are fig_* now; a
# downstream that still calls kiln_* compiles unedited because this pulls in
# one header of macros ahead of its first line.
#
# A force-include rather than an #include inside each public header, for two
# reasons. It is ONE line, so ending the train is deleting this line and
# kiln_compat.h and nothing else — section 9 step 6 is a two-file diff rather
# than a sweep of 63 headers. And it cannot be half-applied: a game gets the
# whole shim or none of it, instead of whichever headers it happened to
# include first.
#
# This deliberately does NOT apply to libkiln.a's own build (engine/Makefile
# does not include this file). The engine compiles against its own real names,
# so a kiln_* that creeps back into engine sources fails there rather than
# being silently bridged — which is the same reason nix/checks/kiln-names.nix
# exists for the last rename this project did.
