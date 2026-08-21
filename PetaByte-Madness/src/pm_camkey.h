// SPDX-License-Identifier: MPL-2.0
//
// pm_camkey.h — a shim. The keyframe and its curve now live in the engine, as
// engine/src/kiln/kiln_camkey.h.
//
// They moved when Forge's CAM mode became a fourth consumer. The invariant that
// made this one implementation in the first place is unchanged and is the whole
// point: the runtime (pm_demo.c), the spatial overlay (pm_debug.c), the native
// validator (pm_cine_lint.c) and now the editor must fly and measure the SAME
// curve. A validator — or an author — working against a curve that merely
// resembles the rendered one is worse than none, because its numbers look
// authoritative.
//
// This file stays so `PMCamKey` and `pm_camkey_sample` keep reading naturally in
// the game's own sources, and so every existing keyframe table compiles
// untouched. It is two typedefs and a define, not a second implementation.
//
// The include is <kiln/kiln_camkey.h>, the same form every other engine header
// takes, because that is what resolves inside the installed $N64_INST prefix the
// ROM builds against. The native validator has no prefix, so
// nix/checks/pm-cine.nix puts `engine/src` (not `engine/src/kiln`) on its include
// path — which makes the same spelling resolve in both places. Getting this
// wrong fails loudly at compile time, which is the one kind of mistake this
// project does not have to be careful about.

#ifndef PM_CAMKEY_H
#define PM_CAMKEY_H

#include <kiln/kiln_camkey.h>

typedef KilnCamKey PMCamKey;

#define pm_camkey_spline1 kiln_camkey_spline1
#define pm_camkey_sample  kiln_camkey_sample

#endif // PM_CAMKEY_H
