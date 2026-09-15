// SPDX-License-Identifier: MIT
//
// kiln_radio.h — pick one track from a catalog with a boot seed.
//
// A nightlight (or any looping bed) wants a different piece each power-on,
// without pulling newlib rand() or sharing kiln_rng state with dice. The
// pick is kiln_rng_range after mixing two 32-bit sources (typically
// get_ticks() and TICKS_READ()) so a cold boot that always sees ticks==0
// still moves when COUNT has ticked.
//
// Not a mixer. The caller loads rom:/... and plays it.

#ifndef KILN_RADIO_H
#define KILN_RADIO_H

#include <stdint.h>

typedef struct {
    const char *path;      /**< DFS path, e.g. rom:/goldberg.wav64 */
    const char *title;     /**< shown on the overlay, if any        */
    const char *composer;  /**< original work this pad is after     */
} KilnRadioTrack;

/** Mix two 32-bit sources into a non-zero 64-bit seed. */
uint64_t kiln_radio_mix_seed(uint32_t ticks, uint32_t extra);

/** Index in [0, n). n <= 1 returns 0. Same seed, same index. */
int kiln_radio_pick(uint64_t seed, int n);

#endif
