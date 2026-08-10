# SPDX-License-Identifier: MPL-2.0
#
# Builds m64_widget.c NATIVELY against the shim headers in shim/, so the real
# widget code can be rendered on the host. See uipreview.c for why.
#
# -Ishim comes FIRST so <libdragon.h> and <fmath.h> resolve to the shims;
# -I../../engine/src/m64 then finds the real m64_widget.h and m64_gui.h.
CFLAGS  = -std=gnu11 -O1 -g -Wall -Wextra -Ishim -I../../engine/src/m64
LDLIBS  = -lm
SRC     = uipreview.c ../../engine/src/m64/m64_widget.c

uipreview: $(SRC) shim/libdragon.h shim/fmath.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f uipreview *.ppm *.png

.PHONY: clean
