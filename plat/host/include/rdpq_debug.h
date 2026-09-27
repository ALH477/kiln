/* SPDX-License-Identifier: MIT
 *
 * plat/host/include/rdpq_debug.h — libdragon's RDP command-stream validator,
 * on a target that has no RDP command stream.
 *
 * ── Why these are no-ops and not aborts ───────────────────────────────
 * rdpq_debug_start installs a validator that reads back the real command list
 * the RDP is about to execute and complains about illegal state. plat/host
 * does not produce one: host_gfx.c is a coverage rasteriser driven straight
 * from the rdpq_* calls, so there is nothing to validate and nothing a
 * validator could catch that the C call itself did not already do.
 *
 * An abort would be the wrong shape. PetaByte Madness reaches these only under
 * -DPM_RDPQ_VALIDATE (src/pm_veil.c), a flag set deliberately when chasing a
 * TLUT ordering bug — and a build flag that halts the host build is a flag
 * nobody can use. Saying once that there is no RDP here is the honest answer;
 * the console build is where that question gets asked.
 */
#ifndef FIG_HOST_RDPQ_DEBUG_H
#define FIG_HOST_RDPQ_DEBUG_H

#include <stdbool.h>

void rdpq_debug_start(void);
void rdpq_debug_stop(void);
void rdpq_debug_log(bool show);
void rdpq_debug_log_msg(const char *msg);

#endif /* FIG_HOST_RDPQ_DEBUG_H */
