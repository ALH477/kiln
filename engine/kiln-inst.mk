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
