/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_prof.h — TICKS-based frame profiler.
 *
 * Measures CPU time in the VR4300 COP0 Count register (via libdragon's
 * TICKS_READ / TICKS_DISTANCE). This is a coarse, frame-scoped tool: it tells
 * you where the VR4300 is spending its time, not where the RSP is. It exists
 * to catch regressions and to give on-hardware numbers for the cycle-budget
 * gates; wall-clock profiling still needs TICKS on a real console.
 *
 * Usage: call m64_prof_init() once, then bracket sections of the frame loop
 * with M64_PROF_BEGIN(zone) / M64_PROF_END(zone). Call m64_prof_frame_done()
 * once per frame after the last zone ends; this updates the EMA and resets
 * the per-frame accumulators. A console command `prof` prints the latest EMA.
 *
 * The profiler is always present in libm64.a; it only costs space/time when
 * the example calls the init/begin/end functions.
 */
#ifndef M64_PROF_H
#define M64_PROF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fixed zones. Add more only if they earn their keep — every zone adds a
 *  little per-frame bookkeeping. */
enum {
    M64_PROF_UPDATE, /* input + actor/game updates + scene culling */
    M64_PROF_SCENE,  /* 3D draw (between m64_scene_begin and m64_gui_begin) */
    M64_PROF_GUI,    /* 2D draw (between m64_gui_begin and m64_gui_end) */
    M64_PROF_AUDIO,  /* mixer pump after m64_frame_end */
    M64_PROF_DEBUG,  /* console draw / debug overlay */
    M64_PROF_TOTAL,  /* whole frame, computed as the sum of the others */
    M64_PROF_COUNT
};

/** Convenience macros — they are just function calls, so a debugger stack
 *  trace stays readable. */
#define M64_PROF_BEGIN(z) m64_prof_begin(z)
#define M64_PROF_END(z)   m64_prof_end(z)

void m64_prof_init(void);
void m64_prof_begin(int zone);
void m64_prof_end(int zone);
void m64_prof_frame_done(void); /* update EMA + reset */

const char *m64_prof_label(int zone);
float m64_prof_ms(int zone);   /* EMA milliseconds */
float m64_prof_frac(int zone); /* zone / total EMA [0,1] */
void m64_prof_print(void);     /* logs all zones to m64_console */

#ifdef __cplusplus
}
#endif

#endif /* M64_PROF_H */