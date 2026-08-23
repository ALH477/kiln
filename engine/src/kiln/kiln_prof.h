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
 * Usage: call kiln_prof_init() once, then bracket sections of the frame loop
 * with KILN_PROF_BEGIN(zone) / KILN_PROF_END(zone). Call kiln_prof_frame_done()
 * once per frame after the last zone ends; this updates the EMA and resets
 * the per-frame accumulators. A console command `prof` prints the latest EMA.
 *
 * The profiler is always present in libkiln.a; it only costs space/time when
 * the example calls the init/begin/end functions.
 */
#ifndef KILN_PROF_H
#define KILN_PROF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fixed zones. Add more only if they earn their keep — every zone adds a
 *  little per-frame bookkeeping. */
enum {
    KILN_PROF_UPDATE, /* input + actor/game updates + scene culling */
    KILN_PROF_SCENE,  /* 3D draw (between kiln_scene_begin and kiln_gui_begin) */
    KILN_PROF_GUI,    /* 2D draw (between kiln_gui_begin and kiln_gui_end) */
    KILN_PROF_AUDIO,  /* mixer pump after kiln_frame_end */
    KILN_PROF_DEBUG,  /* console draw / debug overlay */
    KILN_PROF_TOTAL,  /* whole frame, computed as the sum of the others */
    KILN_PROF_COUNT
};

/** Convenience macros — they are just function calls, so a debugger stack
 *  trace stays readable. */
#define KILN_PROF_BEGIN(z) kiln_prof_begin(z)
#define KILN_PROF_END(z)   kiln_prof_end(z)

void kiln_prof_init(void);
void kiln_prof_begin(int zone);
void kiln_prof_end(int zone);
void kiln_prof_frame_done(void); /* update EMA + reset */

const char *kiln_prof_label(int zone);
float kiln_prof_ms(int zone);   /* EMA milliseconds */
float kiln_prof_frac(int zone); /* zone / total EMA [0,1] */
void kiln_prof_print(void);     /* logs all zones to kiln_console */

#ifdef __cplusplus
}
#endif

#endif /* KILN_PROF_H */