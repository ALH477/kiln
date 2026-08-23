/* SPDX-License-Identifier: MIT
 *
 * kiln_prof.c — see kiln_prof.h for the design.
 */
#include "kiln_prof.h"

#include <kiln/kiln_console.h>

#include <libdragon.h>

#include <stdbool.h>
#include <stdio.h>

/* ── State ──────────────────────────────────────────────────────────────── */

static struct {
    uint32_t begin;
    uint32_t accum;
    float    ema;     /* exponential moving average in milliseconds */
    bool     active;
} g_prof[KILN_PROF_COUNT];

static bool g_init;
static int  g_frame;

/* Half-life of roughly 30 frames for the EMA. */
static const float EMA_ALPHA = 0.05f;

/* Labels must stay in sync with the enum in kiln_prof.h. */
static const char *const LABELS[KILN_PROF_COUNT] = {
    [KILN_PROF_UPDATE] = "update",
    [KILN_PROF_SCENE]  = "scene ",
    [KILN_PROF_GUI]    = "gui   ",
    [KILN_PROF_AUDIO]  = "audio ",
    [KILN_PROF_DEBUG]  = "debug ",
    [KILN_PROF_TOTAL]  = "total ",
};

/* ── API ───────────────────────────────────────────────────────────────── */

void kiln_prof_init(void)
{
    for (int i = 0; i < KILN_PROF_COUNT; i++) {
        g_prof[i].begin = 0;
        g_prof[i].accum = 0;
        g_prof[i].ema = 0.0f;
        g_prof[i].active = false;
    }
    g_init = true;
    g_frame = 0;
}

void kiln_prof_begin(int zone)
{
    if (!g_init) return;
    if (zone < 0 || zone >= KILN_PROF_COUNT) return;
    g_prof[zone].begin = TICKS_READ();
    g_prof[zone].active = true;
}

void kiln_prof_end(int zone)
{
    if (!g_init) return;
    if (zone < 0 || zone >= KILN_PROF_COUNT) return;
    if (!g_prof[zone].active) return;
    uint32_t now = TICKS_READ();
    g_prof[zone].accum += TICKS_DISTANCE(g_prof[zone].begin, now);
    g_prof[zone].active = false;
}

void kiln_prof_frame_done(void)
{
    if (!g_init) return;

    /* total = sum of measured zones (excluding the total slot itself). */
    uint32_t total = 0;
    for (int i = 0; i < KILN_PROF_COUNT; i++)
        if (i != KILN_PROF_TOTAL) total += g_prof[i].accum;
    g_prof[KILN_PROF_TOTAL].accum = total;

    /* Update EMAs in milliseconds. */
    for (int i = 0; i < KILN_PROF_COUNT; i++) {
        float ms = (float)g_prof[i].accum * 1000.0f / TICKS_PER_SECOND;
        if (g_frame == 0) g_prof[i].ema = ms;
        else g_prof[i].ema += EMA_ALPHA * (ms - g_prof[i].ema);
        g_prof[i].accum = 0;
        g_prof[i].active = false;
    }
    g_frame++;
}

const char *kiln_prof_label(int zone)
{
    if (zone >= 0 && zone < KILN_PROF_COUNT) return LABELS[zone];
    return "???";
}

float kiln_prof_ms(int zone)
{
    if (!g_init || zone < 0 || zone >= KILN_PROF_COUNT) return 0.0f;
    return g_prof[zone].ema;
}

float kiln_prof_frac(int zone)
{
    if (!g_init) return 0.0f;
    if (zone < 0 || zone >= KILN_PROF_COUNT) return 0.0f;
    float total = g_prof[KILN_PROF_TOTAL].ema;
    if (total <= 0.0f) return 0.0f;
    return g_prof[zone].ema / total;
}

void kiln_prof_print(void)
{
    if (!g_init) {
        kiln_console_log("prof: not initialised");
        return;
    }
    kiln_console_log("prof (ema over %d frames):", g_frame);
    float total_ms = g_prof[KILN_PROF_TOTAL].ema;
    for (int i = 0; i < KILN_PROF_COUNT; i++) {
        float ms = g_prof[i].ema;
        int pct = (int)(kiln_prof_frac(i) * 100.0f + 0.5f);
        kiln_console_log("  %s %6.2f ms (%3d%%)", LABELS[i], ms,
                        total_ms > 0.0f ? pct : 0);
    }
}