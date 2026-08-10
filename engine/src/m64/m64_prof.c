/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_prof.c — see m64_prof.h for the design.
 */
#include "m64_prof.h"

#include <m64/m64_console.h>

#include <libdragon.h>

#include <stdbool.h>
#include <stdio.h>

/* ── State ──────────────────────────────────────────────────────────────── */

static struct {
    uint32_t begin;
    uint32_t accum;
    float    ema;     /* exponential moving average in milliseconds */
    bool     active;
} g_prof[M64_PROF_COUNT];

static bool g_init;
static int  g_frame;

/* Half-life of roughly 30 frames for the EMA. */
static const float EMA_ALPHA = 0.05f;

/* Labels must stay in sync with the enum in m64_prof.h. */
static const char *const LABELS[M64_PROF_COUNT] = {
    [M64_PROF_UPDATE] = "update",
    [M64_PROF_SCENE]  = "scene ",
    [M64_PROF_GUI]    = "gui   ",
    [M64_PROF_AUDIO]  = "audio ",
    [M64_PROF_DEBUG]  = "debug ",
    [M64_PROF_TOTAL]  = "total ",
};

/* ── API ───────────────────────────────────────────────────────────────── */

void m64_prof_init(void)
{
    for (int i = 0; i < M64_PROF_COUNT; i++) {
        g_prof[i].begin = 0;
        g_prof[i].accum = 0;
        g_prof[i].ema = 0.0f;
        g_prof[i].active = false;
    }
    g_init = true;
    g_frame = 0;
}

void m64_prof_begin(int zone)
{
    if (!g_init) return;
    if (zone < 0 || zone >= M64_PROF_COUNT) return;
    g_prof[zone].begin = TICKS_READ();
    g_prof[zone].active = true;
}

void m64_prof_end(int zone)
{
    if (!g_init) return;
    if (zone < 0 || zone >= M64_PROF_COUNT) return;
    if (!g_prof[zone].active) return;
    uint32_t now = TICKS_READ();
    g_prof[zone].accum += TICKS_DISTANCE(g_prof[zone].begin, now);
    g_prof[zone].active = false;
}

void m64_prof_frame_done(void)
{
    if (!g_init) return;

    /* total = sum of measured zones (excluding the total slot itself). */
    uint32_t total = 0;
    for (int i = 0; i < M64_PROF_COUNT; i++)
        if (i != M64_PROF_TOTAL) total += g_prof[i].accum;
    g_prof[M64_PROF_TOTAL].accum = total;

    /* Update EMAs in milliseconds. */
    for (int i = 0; i < M64_PROF_COUNT; i++) {
        float ms = (float)g_prof[i].accum * 1000.0f / TICKS_PER_SECOND;
        if (g_frame == 0) g_prof[i].ema = ms;
        else g_prof[i].ema += EMA_ALPHA * (ms - g_prof[i].ema);
        g_prof[i].accum = 0;
        g_prof[i].active = false;
    }
    g_frame++;
}

const char *m64_prof_label(int zone)
{
    if (zone >= 0 && zone < M64_PROF_COUNT) return LABELS[zone];
    return "???";
}

float m64_prof_ms(int zone)
{
    if (!g_init || zone < 0 || zone >= M64_PROF_COUNT) return 0.0f;
    return g_prof[zone].ema;
}

float m64_prof_frac(int zone)
{
    if (!g_init) return 0.0f;
    if (zone < 0 || zone >= M64_PROF_COUNT) return 0.0f;
    float total = g_prof[M64_PROF_TOTAL].ema;
    if (total <= 0.0f) return 0.0f;
    return g_prof[zone].ema / total;
}

void m64_prof_print(void)
{
    if (!g_init) {
        m64_console_log("prof: not initialised");
        return;
    }
    m64_console_log("prof (ema over %d frames):", g_frame);
    float total_ms = g_prof[M64_PROF_TOTAL].ema;
    for (int i = 0; i < M64_PROF_COUNT; i++) {
        float ms = g_prof[i].ema;
        int pct = (int)(m64_prof_frac(i) * 100.0f + 0.5f);
        m64_console_log("  %s %6.2f ms (%3d%%)", LABELS[i], ms,
                        total_ms > 0.0f ? pct : 0);
    }
}