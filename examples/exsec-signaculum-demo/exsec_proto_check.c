// SPDX-License-Identifier: MIT
//
// COMPILED, NEVER LINKED. This translation unit exists so that a prototype
// in kiln_soft3d.h that disagrees with the generated definition is a build
// error. It includes the engine module's header, then the console-row
// generated unit itself; a mismatch is "conflicting types". Linking it
// would define exs_signaculum_pingue twice, which is why the Makefile
// builds it as a prerequisite of `all` and leaves it out of the ELF's
// objects. (The same pair compiled for the HOST row happens for free every
// time the host tier compiles kiln_soft3d.c: it includes its own header
// then gen/signaculum_x86_64.gen.c. This TU is the console-row twin.)
//
// The exsec-streamdb-demo/exsec_proto_check.c precedent, verbatim in shape.

#include <kiln/kiln_soft3d.h>
#include "gen/signaculum_mips64.gen.c"
