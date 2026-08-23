# SPDX-License-Identifier: MIT
#
# The store probe. Its own tiny ROM rather than a mode inside Forge, because the
# thing it is proving is a precondition of Forge existing: if this fails the
# editor still runs, but through a different backend, and you want to know that
# before you spend an hour building a level into a save that cannot be written.
#
# Include order is n64.mk, t3d.mk, kiln.mk — each prepends its own -l, so the
# last included ends up leftmost, which is what the dependency direction wants
# (libkiln calls Tiny3D calls libdragon). t3d.mk is included even though this ROM
# draws no 3D: -lkiln's members that reference Tiny3D are never pulled in by this
# link, but keeping the include order identical to every other ROM here means
# one less thing that is different about the debug tool.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk
include $(N64_INST)/include/t3d.mk
include $(N64_INST)/include/kiln.mk

N64_CFLAGS += -std=gnu2x -Wno-error

OBJS = $(BUILD_DIR)/main.o

all: forge-selftest.z64

$(BUILD_DIR)/forge-selftest.elf: $(OBJS)

forge-selftest.z64: N64_ROM_TITLE = "Kiln Forge Probe"

clean:
	rm -rf $(BUILD_DIR) forge-selftest.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
