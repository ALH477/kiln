# SPDX-License-Identifier: MIT
#
# Interceptor demo ROM: one screen that tries to do everything the engine can
# do in one go, so the .t3dm hero prop has somewhere to live that exercises
# every layer. Loads the model, scripts a flight path, plays music, fires
# SFX, draws engine streaks inside the 3D pass, and renders the lineage HUD.
#
# assets staged into filesystem/ by mkN64Rom's `assets` argument:
#   models/interceptor.t3dm   -> the hero prop
#   sfx/blip.wav64            -> SFX (re-used as engine whoosh)
#   music/test.xm64           -> music
#
# Same include order as examples/engine: n64.mk, t3d.mk, kiln.mk. libkiln calls
# into Tiny3D, which calls into libdragon, so the link line must be Tiny3D
# left of libkiln.

BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: interceptor-demo.z64

$(BUILD_DIR)/interceptor-demo.elf: $(OBJS)

interceptor-demo.z64: N64_ROM_TITLE = "Kiln Interceptor"
interceptor-demo.z64: $(BUILD_DIR)/interceptor-demo.dfs

clean:
	rm -rf $(BUILD_DIR) interceptor-demo.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean