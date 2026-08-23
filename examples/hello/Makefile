# SPDX-License-Identifier: MIT
#
# The canonical libdragon project shape: set BUILD_DIR, include n64.mk, declare
# your objects, and name the .z64. Everything else (ROM header, ELF compression,
# symbol table, IPL3) is n64.mk's job.
#
# N64_INST and N64_GCCPREFIX come from the environment — the devShell and
# mkN64Rom both set them. Outside Nix you would export them yourself.

BUILD_DIR = build
include $(N64_INST)/include/n64.mk

OBJS = $(BUILD_DIR)/hello.o

all: hello.z64

$(BUILD_DIR)/hello.elf: $(OBJS)

hello.z64: N64_ROM_TITLE = "Kiln Hello"

clean:
	rm -rf $(BUILD_DIR) hello.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
