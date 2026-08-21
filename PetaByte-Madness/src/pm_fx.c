// SPDX-License-Identifier: MPL-2.0
//
// pm_fx.c — see pm_fx.h.

#include "pm_fx.h"

#include <kiln/kiln_gui.h>

#include "pm_cine.h"  // the cue trace; compiles away without KILN_DEBUG

// ── Shake noise ────────────────────────────────────────────────────────
// 32 entries of a rough sine, in 8.8 fixed point over [-1, 1]. Three
// lookups at coprime strides per axis give a wobble with no visible period
// at shake durations, for the cost of three array reads.
//
// Coprime matters: at equal strides all three axes would move together and
// the "shake" would be a straight line in a fixed direction.
static const int16_t WOBBLE[32] = {
       0,   49,   97,  141,  181,  213,  237,  252,
     256,  252,  237,  213,  181,  141,   97,   49,
       0,  -49,  -97, -141, -181, -213, -237, -252,
    -256, -252, -237, -213, -181, -141,  -97,  -49,
};

static float wobble(int phase, int stride)
{
    return (float)WOBBLE[(phase * stride) & 31] * (1.0f / 256.0f);
}

// ── State ──────────────────────────────────────────────────────────────
static float g_clock;          // real-time seconds, for the noise phase

static float g_letterbox;      // current 0..1
static float g_letterbox_want;

static float g_shake_amount;   // peak displacement, world units
static float g_shake_left;
static float g_shake_total;

static color_t g_flash_color;
static float   g_flash_left;
static float   g_flash_total;

static float g_hitstop_left;

static float g_binary_left;

#define LETTERBOX_SPEED 2.5f   // 1/seconds -> ~0.4 s in or out
#define LETTERBOX_FRAC  0.12f  // bar height as a fraction of the screen

void pm_fx_reset(void)
{
    g_letterbox = g_letterbox_want = 0.0f;
    g_shake_amount = g_shake_left = g_shake_total = 0.0f;
    g_flash_left = g_flash_total = 0.0f;
    g_hitstop_left = 0.0f;
    g_binary_left = 0.0f;
}

void pm_fx_update(float dt)
{
    g_clock += dt;

    if (g_letterbox < g_letterbox_want) {
        g_letterbox += LETTERBOX_SPEED * dt;
        if (g_letterbox > g_letterbox_want) g_letterbox = g_letterbox_want;
    } else if (g_letterbox > g_letterbox_want) {
        g_letterbox -= LETTERBOX_SPEED * dt;
        if (g_letterbox < g_letterbox_want) g_letterbox = g_letterbox_want;
    }

    if (g_shake_left > 0.0f)   g_shake_left   -= dt;
    if (g_flash_left > 0.0f)   g_flash_left   -= dt;
    if (g_hitstop_left > 0.0f) g_hitstop_left -= dt;
    if (g_binary_left > 0.0f)  g_binary_left  -= dt;
}

// ── Letterbox ──────────────────────────────────────────────────────────
void pm_fx_letterbox(float target)
{
    PM_CUE(target > 0.0f ? "fx.bars.in" : "fx.bars.out");
    g_letterbox_want = target < 0.0f ? 0.0f : (target > 1.0f ? 1.0f : target);
}

int pm_fx_letterbox_settled(void)
{
    const float d = g_letterbox - g_letterbox_want;
    return (d > -0.01f && d < 0.01f);
}

// ── Shake ──────────────────────────────────────────────────────────────
void pm_fx_shake(float amount, float seconds)
{
    // Stronger wins, rather than summing. Two hits landing together should
    // read as one bigger hit, not as double the displacement.
    if (amount <= g_shake_amount && g_shake_left > 0.0f) return;
    // Recorded only when it actually takes: pm_intake.c re-arms a small shake
    // EVERY FRAME while the slab motor runs (so the vibration outlives the
    // decay), and a trace that logged all 750 of those would bury the four
    // beats worth reading.
    PM_CUE("fx.shake");
    g_shake_amount = amount;
    g_shake_left = g_shake_total = seconds;
}

void pm_fx_apply_camera(KilnScene *scene)
{
    if (g_shake_left <= 0.0f || g_shake_total <= 0.0f) return;

    // Linear decay. A shake that tails off exponentially keeps twitching
    // after the moment has passed; one that stops cleanly reads as an
    // impact with an end.
    const float k = (g_shake_left / g_shake_total) * g_shake_amount;
    const int phase = (int)(g_clock * 90.0f);

    scene->cam_pos.v[0] += wobble(phase, 1) * k;
    scene->cam_pos.v[1] += wobble(phase, 3) * k;
    scene->cam_pos.v[2] += wobble(phase, 7) * k;
    // The look-at moves with the eye, so the shake is the whole camera
    // rattling rather than it pivoting about a fixed target — pivoting
    // reads as the WORLD shaking, which is a different (and much cheaper
    // looking) effect.
    scene->cam_target.v[0] += wobble(phase, 1) * k * 0.6f;
    scene->cam_target.v[1] += wobble(phase, 3) * k * 0.6f;
}

// ── Flash ──────────────────────────────────────────────────────────────
void pm_fx_flash(color_t c, float seconds)
{
    PM_CUE("fx.flash");
    g_flash_color = c;
    g_flash_left = g_flash_total = seconds;
}

// ── Hit-stop ───────────────────────────────────────────────────────────
void pm_fx_hitstop(float seconds)
{
    if (seconds <= g_hitstop_left) return;
    PM_CUE("fx.hitstop");
    g_hitstop_left = seconds;
}

float pm_fx_time_scale(void) { return g_hitstop_left > 0.0f ? 0.0f : 1.0f; }

// ── Binary scroll ──────────────────────────────────────────────────────
void pm_fx_binary(float seconds) { PM_CUE("fx.binary"); g_binary_left = seconds; }
int  pm_fx_binary_active(void)   { return g_binary_left > 0.0f; }

// "MADNESS" in ASCII, one byte per row, so a paused frame decodes. The
// rest of each row is filler that changes every frame — the word is the
// only stable thing on screen, which is what makes it readable at speed
// without being legible at a glance.
static const uint8_t MADNESS[] = { 'M', 'A', 'D', 'N', 'E', 'S', 'S' };

static void draw_binary(int w, int h)
{
    const color_t ink = RGBA32(0x30, 0xE0, 0x48, 0xFF);
    const int rows = h / 12;
    // Scrolls upward fast — 40 rows a second, so no row is readable but
    // the column of them reads as data moving.
    const int scroll = (int)(g_clock * 40.0f);

    char line[41];
    for (int r = 0; r < rows; r++) {
        const int idx = (r + scroll);
        const uint8_t byte = MADNESS[idx % (int)sizeof MADNESS];
        // Eight bits of the letter, then filler derived from the row so it
        // is stable within a frame and different between them.
        for (int b = 0; b < 8; b++) line[b] = (byte & (0x80 >> b)) ? '1' : '0';
        line[8] = ' ';
        uint32_t noise = (uint32_t)(idx * 2654435761u) ^ (uint32_t)(scroll * 40503u);
        for (int c = 9; c < 40; c++) {
            noise = noise * 1103515245u + 12345u;
            line[c] = (noise >> 16 & 1) ? '1' : '0';
        }
        line[40] = '\0';
        kiln_gui_text(4, r * 12 + 8, ink, "%s", line);
    }
}

// ── Draw ───────────────────────────────────────────────────────────────
void pm_fx_draw(int w, int h)
{
    if (g_binary_left > 0.0f) draw_binary(w, h);

    if (g_letterbox > 0.001f) {
        const int bar = (int)(h * LETTERBOX_FRAC * g_letterbox);
        const color_t black = RGBA32(0, 0, 0, 255);
        kiln_gui_rect(0, 0, w, bar, black);
        kiln_gui_rect(0, h - bar, w, bar, black);
    }

    if (g_flash_left > 0.0f && g_flash_total > 0.0f) {
        const float k = g_flash_left / g_flash_total;
        const uint8_t a = (uint8_t)(g_flash_color.a * k);
        kiln_gui_rect(0, 0, w, h,
                     RGBA32(g_flash_color.r, g_flash_color.g,
                            g_flash_color.b, a));
    }
}
