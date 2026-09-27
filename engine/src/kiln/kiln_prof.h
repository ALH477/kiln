/* SPDX-License-Identifier: MIT
 *
 * kiln_prof.h — TICKS-based frame profiler.
 *
 * Measures CPU time in the VR4300 COP0 Count register (via libdragon's
 * TICKS_READ / TICKS_DISTANCE). This is a coarse, frame-scoped tool: it tells
 * you where the VR4300 is spending its time, not where the RSP is. It exists
 * to catch regressions and to give on-hardware numbers for the cycle-budget
 * gates; wall-clock profiling still needs TICKS on a real console.
 *
 * Usage: call fig_prof_init() once, then bracket sections of the frame loop
 * with FIG_PROF_BEGIN(zone) / FIG_PROF_END(zone). Call fig_prof_frame_done()
 * once per frame after the last zone ends; this updates the EMA and resets
 * the per-frame accumulators. A console command `prof` prints the latest EMA.
 *
 * The profiler is always present in libfigulina.a; it only costs space/time when
 * the example calls the init/begin/end functions.
 */
#ifndef FIG_PROF_H
#define FIG_PROF_H


/* The prefix migration train (docs/NAMING.md section 9 step 2). Pulled in by
 * every public header (a quoted include, so it resolves both in this tree and
 * in the installed include/kiln prefix) rather than force-included by
 * kiln-inst.mk, because a
 * force-include only reaches builds that include that file — a Nix check or a
 * host build compiling a downstream's sources directly never saw it, and
 * PetaByte-Madness' pm-cine check is what proved that. Deleting the train is
 * still a scripted one-line removal from these headers plus the file itself.
 */
#include "kiln_compat.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fixed zones. Add more only if they earn their keep — every zone adds a
 *  little per-frame bookkeeping. */
enum {
    FIG_PROF_UPDATE, /* input + actor/game updates + scene culling */
    FIG_PROF_SCENE,  /* 3D draw (between fig_scene_begin and fig_gui_begin) */
    FIG_PROF_GUI,    /* 2D draw (between fig_gui_begin and fig_gui_end) */
    FIG_PROF_AUDIO,  /* mixer pump after fig_frame_end */
    FIG_PROF_DEBUG,  /* console draw / debug overlay */
    FIG_PROF_TOTAL,  /* whole frame, computed as the sum of the others */
    FIG_PROF_COUNT
};

/** Convenience macros — they are just function calls, so a debugger stack
 *  trace stays readable. */
#define FIG_PROF_BEGIN(z) fig_prof_begin(z)
#define FIG_PROF_END(z)   fig_prof_end(z)

void fig_prof_init(void);
void fig_prof_begin(int zone);
void fig_prof_end(int zone);
void fig_prof_frame_done(void); /* update EMA + reset */

const char *fig_prof_label(int zone);
float fig_prof_ms(int zone);   /* EMA milliseconds */
float fig_prof_frac(int zone); /* zone / total EMA [0,1] */
void fig_prof_print(void);     /* logs all zones to fig_console */

#ifdef __cplusplus
}
#endif

#endif /* FIG_PROF_H */