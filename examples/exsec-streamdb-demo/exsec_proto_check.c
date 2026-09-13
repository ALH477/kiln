// SPDX-License-Identifier: MIT
//
// COMPILED, NEVER LINKED. This translation unit exists so that a prototype in
// exsec_streamdb.h that disagrees with the generated definition is a build
// error. It includes the header, then the generated unit itself; a mismatch is
// "conflicting types". Linking it would define every reader symbol twice,
// which is why the Makefile builds it as a prerequisite of `all` and leaves it
// out of the ELF's objects.

#include "exsec_streamdb.h"
#include "lector_streamdb.gen.c"
