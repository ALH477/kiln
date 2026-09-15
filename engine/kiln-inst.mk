# SPDX-License-Identifier: MIT
#
# Included by a ROM's Makefile after n64.mk and t3d.mk. Order matters at link
# time: libkiln calls into Tiny3D, so -lkiln has to come before -lt3d, which
# t3d.mk has already prepended to N64_LDFLAGS.
N64_LDFLAGS := -lkiln $(N64_LDFLAGS)

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
