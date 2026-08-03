# SPDX-License-Identifier: MPL-2.0
#
# Included by a ROM's Makefile after n64.mk and t3d.mk. Order matters at link
# time: libm64 calls into Tiny3D, so -lm64 has to come before -lt3d, which
# t3d.mk has already prepended to N64_LDFLAGS.
N64_LDFLAGS := -lm64 $(N64_LDFLAGS)
