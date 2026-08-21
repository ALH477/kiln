// SPDX-License-Identifier: MPL-2.0
//
// pm_narration.c — see pm_narration.h.

#include "pm_narration.h"

#include <libdragon.h>
#include <string.h>

#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_audio.h>

#include "pm_screens.h"  // PM_CH_STORY
#include "pm_hud.h"      // PM_UI_*

#define STREAM_PATH "rom:/music/narration_stream.wav64"

// Slower than kiln_dialogue's 30 chars/sec on purpose — this is a crawl
// under a four-minute score, not a conversation. See narration_setup for
// how this and the score's own measured length (254.13 s, ffprobe'd
// against ostafterstart.mp3 — see the plan this shipped from) combine into
// a per-page hold: type time is derived from this rate, and whatever's
// left of the shot's duration is spent holding, split evenly across pages.
#define TYPE_RATE 12.0f

typedef struct {
    const char *lines[3];
    int         line_count;
} NarrationPage;

// Hand-broken at the same ~36-char budget kiln_dialogue.h documents for the
// debug font at 320x240. Five pages, one story: MedCorp made him obsolete,
// he turned on them, they took his daughter, he found her, and now this.
static const NarrationPage PAGES[] = {
    { { "Dr. Horner was MedCorp's",
        "best biologist -",
        "the only one who could" }, 3 },
    { { "create what he created.",
        "So MedCorp built",
        "deterministic systems" }, 3 },
    { { "to replace him.",
        "When he pushed back,",
        "they took his daughter" }, 3 },
    { { "to bring him back in line.",
        "He has triangulated",
        "where they're holding her." }, 3 },
    { { "Now he prepares to endure",
        "PETABYTE MADNESS.",
        NULL }, 2 },
};
#define PAGE_COUNT ((int)(sizeof PAGES / sizeof PAGES[0]))

static int   g_page;
static int   g_line_chars[3];
static float g_char_timer;
static float g_hold_t;
static float g_hold_per_page;
static int   g_stream = -1;

static void narration_setup(void)
{
    int total_chars = 0;
    for (int p = 0; p < PAGE_COUNT; p++)
        for (int l = 0; l < PAGES[p].line_count; l++)
            total_chars += (int)strlen(PAGES[p].lines[l]);

    // duration is authored to match ostafterstart.mp3's real length, not
    // guessed — see the header comment. Deriving the hold from IT (rather
    // than hand-picking a hold and letting duration drift from the score)
    // means adding or trimming a line never desyncs the two.
    const float type_time = (float)total_chars / TYPE_RATE;
    g_hold_per_page = (pm_narration_shot.duration - type_time)
                      / (float)PAGE_COUNT;
    if (g_hold_per_page < 0.0f) g_hold_per_page = 0.0f;

    g_page = 0;
    memset(g_line_chars, 0, sizeof g_line_chars);
    g_char_timer = 0.0f;
    g_hold_t = 0.0f;

    g_stream = kiln_dfs_exists(STREAM_PATH) ? kiln_sfx_load(STREAM_PATH) : -1;
    if (g_stream >= 0) {
        // Priority 255 on the shared story channel: nothing else in pm_sfx
        // can steal it mid-crawl. See PM_CH_STORY's comment in pm_screens.h
        // for why sharing the channel with the surgery OST is safe.
        kiln_sfx_play(g_stream, PM_CH_STORY, 255);
    } else {
        debugf("pm_narration: no %s, running silent\n", STREAM_PATH);
    }
}

static void narration_teardown(void)
{
    if (g_stream >= 0) {
        kiln_sfx_stop(PM_CH_STORY);
        g_stream = -1;
    }
}

static void narration_update(float elapsed, float dt)
{
    (void)elapsed;
    const NarrationPage *pg = &PAGES[g_page];

    int all_revealed = 1;
    for (int l = 0; l < pg->line_count; l++) {
        if (g_line_chars[l] < (int)strlen(pg->lines[l])) {
            all_revealed = 0;
            break;
        }
    }

    if (!all_revealed) {
        g_char_timer += dt;
        const float step = 1.0f / TYPE_RATE;
        // One character per step, always the first not-yet-full line, so
        // the page fills top to bottom rather than all lines growing at
        // once — the same "one line at a time" shape kiln_dialogue uses,
        // just auto-paced instead of A-gated.
        while (g_char_timer >= step) {
            g_char_timer -= step;
            int advanced = 0;
            for (int l = 0; l < pg->line_count; l++) {
                const int len = (int)strlen(pg->lines[l]);
                if (g_line_chars[l] < len) {
                    g_line_chars[l]++;
                    advanced = 1;
                    break;
                }
            }
            if (!advanced) break;
        }
    } else {
        g_hold_t += dt;
        if (g_hold_t >= g_hold_per_page && g_page + 1 < PAGE_COUNT) {
            g_page++;
            memset(g_line_chars, 0, sizeof g_line_chars);
            g_hold_t = 0.0f;
        }
    }
}

const PMDemoShot pm_narration_shot = {
    .name     = "narration",
    .duration = 254.13f,  // ostafterstart.mp3's measured length
    .setup    = narration_setup,
    .teardown = narration_teardown,
    .update   = narration_update,
    // .draw is NULL: nothing to show in the 3D pass. pm_env_interior's
    // dark clear colour (RGBA32(10,10,24,255), same tone kiln_dialogue's own
    // panel uses) is close enough to a black backdrop that this shot never
    // needed to fight the environment system for one.
};

void pm_narration_draw2d(int w, int h)
{
    const NarrationPage *pg = &PAGES[g_page];
    const int line_h = 16;
    const int y0 = h / 2 - (pg->line_count * line_h) / 2;

    for (int l = 0; l < pg->line_count; l++) {
        const int n = g_line_chars[l];
        if (n <= 0 || !pg->lines[l]) continue;

        const int len = (int)strlen(pg->lines[l]);
        char buf[40];
        int shown = n < len ? n : len;
        if (shown > (int)sizeof buf - 1) shown = (int)sizeof buf - 1;
        memcpy(buf, pg->lines[l], shown);
        buf[shown] = 0;

        // Centred on the FULL line's width, not the revealed prefix's —
        // centring on a growing substring would walk the text sideways as
        // it types. ~7 px/char is the same debug-font guess draw_title and
        // draw_file already lean on for their own centred lines.
        const int tw = len * 7;
        kiln_gui_text(w / 2 - tw / 2, y0 + l * line_h, PM_UI_INK, "%s", buf);
    }
}
