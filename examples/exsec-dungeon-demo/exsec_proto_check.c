// SPDX-License-Identifier: MIT
//
// COMPILED, NEVER LINKED. This translation unit exists so that a prototype in
// exsec_dungeon.h that disagrees with the generated definition is a build
// error: it includes the header, then the generated unit itself, and a mismatch
// is "conflicting types". Linking it would define every symbol twice, which is
// why the Makefile builds it as a prerequisite of `all` and leaves it out of the
// ELF's objects.

#include "exsec_dungeon.h"
#include "dungeon_view.h"
#include "dungeon_golden.h"
#include "dungeon_mips64.gen.c"
