// SPDX-License-Identifier: MPL-2.0
//
// pm_cine_lint.h — a shim. The validator now lives in the engine, as
// engine/src/kiln/kiln_camlint.{h,c}.
//
// It moved with kiln_camkey when Forge's CAM mode became a consumer: an editor
// that happily saves a keyframe table the game will refuse is worse than one
// that never checked, because the failure then surfaces two tools away from the
// person who caused it. Forge runs these rules at authoring time, on the same
// code, against the same curve.
//
// The move was a rename rather than a rewrite because the original was already
// written to be movable — `PMCineShot` mirrored a PMDemoShot's fields instead of
// taking one, expressly so this header depended on nothing from the game. Worth
// noticing as a general result: the thing that made it portable was refusing a
// convenient coupling once.
//
// This file keeps the pm_* spelling for the game's own sources, so pm_cine.c,
// pm_debug.c and nix/checks/pm-cine-check.c compile untouched.

#ifndef PM_CINE_LINT_H
#define PM_CINE_LINT_H

#include <kiln/kiln_camlint.h>

// The original pm_cine_lint.h included pm_camkey.h, so its consumers got
// PMCamKey transitively and several rely on that — nix/checks/pm-cine-check.c
// declares `static const PMCamKey ...[]` with no include of its own. A shim has
// to reproduce the include surface it replaces, not just the symbols: the point
// is that nothing downstream changes.
#include "pm_camkey.h"

typedef KilnCamBounds PMCineBounds;
typedef KilnCamReport PMCineReport;
typedef KilnCamShot   PMCineShot;

#define pm_cine_lint       kiln_camlint
#define pm_cine_err_name   kiln_camlint_err_name
#define pm_cine_note_name  kiln_camlint_note_name

#define PM_CINE_ERR_NO_KEYS       KILN_CAMLINT_ERR_NO_KEYS
#define PM_CINE_ERR_TIME_ORDER    KILN_CAMLINT_ERR_TIME_ORDER
#define PM_CINE_ERR_KEY_PAST_END  KILN_CAMLINT_ERR_KEY_PAST_END
#define PM_CINE_ERR_DEGENERATE    KILN_CAMLINT_ERR_DEGENERATE
#define PM_CINE_ERR_FRUSTUM       KILN_CAMLINT_ERR_FRUSTUM
#define PM_CINE_ERR_SUBJECT_CUT   KILN_CAMLINT_ERR_SUBJECT_CUT
#define PM_CINE_ERR_NAN           KILN_CAMLINT_ERR_NAN
#define PM_CINE_ERR_COUNT         KILN_CAMLINT_ERR_COUNT

#define PM_CINE_NOTE_LATE_START   KILN_CAMLINT_NOTE_LATE_START
#define PM_CINE_NOTE_DEAD_TAIL    KILN_CAMLINT_NOTE_DEAD_TAIL
#define PM_CINE_NOTE_OVERSHOOT    KILN_CAMLINT_NOTE_OVERSHOOT
#define PM_CINE_NOTE_HITCH        KILN_CAMLINT_NOTE_HITCH
#define PM_CINE_NOTE_OUTSIDE      KILN_CAMLINT_NOTE_OUTSIDE
#define PM_CINE_NOTE_NEAR_AIM     KILN_CAMLINT_NOTE_NEAR_AIM

#define PM_CINE_OVERSHOOT_FRAC    KILN_CAMLINT_OVERSHOOT_FRAC
#define PM_CINE_HITCH_RATIO       KILN_CAMLINT_HITCH_RATIO
#define PM_CINE_TAIL_FRAC         KILN_CAMLINT_TAIL_FRAC
#define PM_CINE_SAMPLES_PER_SEG   KILN_CAMLINT_SAMPLES_PER_SEG

#endif // PM_CINE_LINT_H
