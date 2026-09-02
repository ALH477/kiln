# SPDX-License-Identifier: MIT
#
# Builds kiln_widget.c and the REAL kiln_gui.c natively, against the shared
# host backend in plat/host. There used to be a private shim/ directory and a
# private software rasteriser here; both are gone, because a preview that
# draws its own pixels is a second implementation of the thing it is previewing
# — see uipreview.c's header for the three ways it disagreed with the first.
#
# This Makefile is the by-hand path, for regenerating the reference images and
# looking at them. The GATE does not use it — nix/checks/kiln-widget.nix goes
# through nix/host.nix like every other host check, which is also where the
# flags that have to be identical across architectures live. Keep the two in
# step or regenerate references with the nix path.
#
# HOST_MATH must point at nix/host-math.nix's output (libdragon's own fm_*,
# built natively). Inside `nix develop` it is already exported; otherwise:
#   make -C tools/uipreview HOST_MATH=$(nix build --no-link --print-out-paths .#host-math)
HOST_MATH ?= $(KILN_HOST_MATH)

PLAT    = ../../plat/host
ENGINE  = ../../engine/src/kiln

# zlib is host_png.c's only third-party dependency. pkg-config rather than a
# bare -lz so this builds outside `nix develop` too, where the headers are not
# on the default include path.
ZLIB_CFLAGS ?= $(shell pkg-config --cflags zlib 2>/dev/null)
ZLIB_LIBS   ?= $(shell pkg-config --libs zlib 2>/dev/null || echo -lz)

# -ffp-contract=off for the same reason nix/host.nix sets it: without it an
# FMA-capable host fuses the rasteriser's edge functions and every triangle
# edge moves by a ULP relative to the committed references.
CFLAGS  = -std=gnu2x -O1 -g -Wall -Wextra -ffp-contract=off \
          -I$(PLAT)/include -I$(HOST_MATH)/include -I$(ENGINE) $(ZLIB_CFLAGS)
LDLIBS  = $(HOST_MATH)/lib/libkilnmath.a $(ZLIB_LIBS) -lm
SRC     = uipreview.c $(ENGINE)/kiln_widget.c $(ENGINE)/kiln_gui.c \
          $(PLAT)/src/host_gfx.c $(PLAT)/src/host_png.c

uipreview: $(SRC)
	@test -n "$(HOST_MATH)" || { \
	  echo "HOST_MATH is unset. See this Makefile's header."; exit 1; }
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f uipreview *.png

.PHONY: clean
