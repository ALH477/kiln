/* SPDX-License-Identifier: MIT
 *
 * shell_web.c — the browser launcher: a canvas, the Gamepad API, Web Audio.
 *
 * ── Why not SDL2 under Emscripten ──────────────────────────────────────
 * -sUSE_SDL=2 downloads its port on first use, and every build in this repo
 * happens inside a Nix sandbox with no network. Building SDL2 from source
 * with emcc would work and is a cmake project's worth of machinery to obtain
 * a window, a key table and an audio sink — which, with the rasterising
 * already done by plat/host, is about a hundred lines of EM_JS. So this is
 * that hundred lines. Everything a launcher is actually allowed to decide —
 * the key map, the stick shape, the frame deadline — lives in
 * kiln_shell_common.c and is shared with shell_sdl.c, so the two backends
 * cannot drift into being two different games.
 *
 * ── Why ASYNCIFY and not emscripten_set_main_loop ──────────────────────
 * Every game loop in this repo is `for (;;)` inside the ROM's own main(), and
 * the loop body is in 22 separate examples/<x>/main.c rather than in the
 * engine — kiln_engine.c deliberately owns the frame bracket and not the
 * loop. There is no single place to invert control, and inverting it in 22
 * places would mean 22 files that build differently for the browser.
 *
 * ASYNCIFY makes the blocking loop legal: the yield goes in the vsync hook,
 * which is called from display_get, which is the exact function the console
 * blocks in waiting for the VI to release a buffer. So the browser build
 * yields where the console waits, the source is unchanged, and the shape of
 * the frame is the same on both. The cost is Asyncify's instrumentation on
 * the wasm, which for a software rasteriser bound by its own inner loops is
 * not where the time goes.
 */
#include "kiln_shell.h"

#include <libdragon.h>
#include <kiln_host.h>

#include <emscripten.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── the canvas ────────────────────────────────────────────────────── */

EM_JS(void, web_open, (int w, int h, const char *title), {
  var k = Module.kiln = Module.kiln || {};
  var c = Module.canvas || document.getElementById('canvas');
  if (!c) {
    c = document.createElement('canvas');
    c.id = 'canvas';
    document.body.appendChild(c);
    Module.canvas = c;
  }
  c.width = w; c.height = h;
  // Nearest-neighbour, always. A 320x240 frame smoothed up to a 1080p canvas
  // is not the frame the gates compare, and the whole reason this backend
  // blits a software-rasterised buffer is that it IS that frame.
  c.style.imageRendering = 'pixelated';
  c.style.width = '100%';
  c.style.maxWidth = (w * 4) + 'px';
  c.style.display = 'block';
  c.style.margin = '0 auto';
  c.style.background = '#000';
  k.ctx = c.getContext('2d', { alpha: false });
  k.img = k.ctx.createImageData(w, h);
  if (title) document.title = UTF8ToString(title);
});

EM_JS(void, web_blit, (const void *fb, int w, int h), {
  var k = Module.kiln;
  if (!k || !k.img) return;
  k.img.data.set(HEAPU8.subarray(fb, fb + w * h * 4));
  k.ctx.putImageData(k.img, 0, 0);
});

/* ── input ─────────────────────────────────────────────────────────── */
/* The key table is written by JS and read by C, one byte per KilnShellKey, so
 * the mapping itself stays in kiln_shell_common.c where shell_sdl.c also
 * reads it. JS only says which physical key sets which slot. */

EM_JS(void, web_input_init, (void), {
  var k = Module.kiln = Module.kiln || {};
  k.keys = new Uint8Array(32);
  // Index order is KilnShellKey in plat/shell/kiln_shell.h.
  k.map = {
    'KeyW':0,'KeyS':1,'KeyA':2,'KeyD':3,
    'ArrowUp':4,'ArrowDown':5,'ArrowLeft':6,'ArrowRight':7,
    'Space':8,'Period':8,'Comma':9,'ShiftLeft':10,'ShiftRight':10,
    'KeyQ':11,'KeyE':12,'Enter':13,
    'KeyI':14,'KeyK':15,'KeyJ':16,'KeyL':17
  };
  var set = function(e, v) {
    var i = k.map[e.code];
    if (i !== undefined) { k.keys[i] = v; e.preventDefault(); }
  };
  window.addEventListener('keydown', function(e) { set(e, 1); });
  window.addEventListener('keyup',   function(e) { set(e, 0); });
  // A browser tab that loses focus keeps the last key down forever otherwise,
  // which reads as the character walking into a wall by itself.
  window.addEventListener('blur', function() { k.keys.fill(0); });
});

EM_JS(void, web_keys, (unsigned char *out, int n), {
  var k = Module.kiln;
  if (!k || !k.keys) return;
  HEAPU8.set(k.keys.subarray(0, n), out);
});

/* Returns a packed pad: bit 0..13 buttons in KilnShellPad order, then two
 * signed bytes of stick in the high half. Packed rather than a struct because
 * an EM_JS return crosses the boundary as one number. */
EM_JS(int, web_gamepad, (void), {
  var pads = navigator.getGamepads ? navigator.getGamepads() : [];
  var p = null;
  for (var i = 0; i < pads.length; i++) if (pads[i] && pads[i].connected) { p = pads[i]; break; }
  if (!p) return 0;
  var b = function(i) { return (p.buttons[i] && p.buttons[i].pressed) ? 1 : 0; };
  var ax = function(i) { var v = p.axes[i] || 0; return Math.abs(v) < 0.1 ? 0 : v; };
  var out = 1;                                  // bit 0: a pad is present
  out |= b(0) << 1;                             // A
  out |= (b(2) | b(1)) << 2;                    // B
  out |= (b(6) > 0 ? 1 : 0) << 3;               // Z  (left trigger)
  out |= b(4) << 4;                             // L
  out |= b(5) << 5;                             // R
  out |= b(9) << 6;                             // Start
  out |= b(12) << 7;  out |= b(13) << 8;        // D-pad up / down
  out |= b(14) << 9;  out |= b(15) << 10;       // D-pad left / right
  var cx = ax(2), cy = ax(3);
  out |= (cy < -0.4 ? 1 : 0) << 11;             // C up
  out |= (cy >  0.4 ? 1 : 0) << 12;             // C down
  out |= (cx < -0.4 ? 1 : 0) << 13;             // C left
  out |= (cx >  0.4 ? 1 : 0) << 14;             // C right
  var sx = Math.round(ax(0) * 90), sy = Math.round(-ax(1) * 90);
  out |= ((sx & 0xFF) << 16);
  out |= ((sy & 0xFF) << 24);
  return out | 0;
});

/* ── audio ─────────────────────────────────────────────────────────── */
/* Scheduled AudioBuffers rather than an AudioWorklet: a worklet needs a
 * second script served alongside the wasm, and this build is meant to be
 * three files. The cursor is kept a little ahead of currentTime, which is the
 * whole of the buffering strategy. */

EM_JS(int, web_audio_open, (int freq), {
  var k = Module.kiln = Module.kiln || {};
  if (k.actx) return 1;
  var AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) return 0;
  k.actx = new AC({ sampleRate: freq });
  k.cursor = 0;
  // Browsers refuse to start audio until a gesture. Resuming on the first
  // one is the standard dance; until then the queue simply never drains and
  // audio_free reports full, which the mixer handles as backpressure.
  var resume = function() { if (k.actx.state === 'suspended') k.actx.resume(); };
  window.addEventListener('pointerdown', resume, { once: false });
  window.addEventListener('keydown', resume, { once: false });
  return 1;
});

/* Seconds of audio already scheduled beyond now, x1000. */
EM_JS(int, web_audio_queued_ms, (void), {
  var k = Module.kiln;
  if (!k || !k.actx) return 0;
  var ahead = k.cursor - k.actx.currentTime;
  return ahead > 0 ? (ahead * 1000) | 0 : 0;
});

EM_JS(void, web_audio_push, (const short *pcm, int frames, int freq), {
  var k = Module.kiln;
  if (!k || !k.actx) return;
  var buf = k.actx.createBuffer(2, frames, freq);
  var l = buf.getChannelData(0), r = buf.getChannelData(1);
  var base = pcm >> 1;
  for (var i = 0; i < frames; i++) {
    l[i] = HEAP16[base + i * 2]     / 32768;
    r[i] = HEAP16[base + i * 2 + 1] / 32768;
  }
  var src = k.actx.createBufferSource();
  src.buffer = buf;
  src.connect(k.actx.destination);
  var now = k.actx.currentTime;
  if (k.cursor < now + 0.02) k.cursor = now + 0.02;
  src.start(k.cursor);
  k.cursor += frames / freq;
});

/* ── hooks ─────────────────────────────────────────────────────────── */

static KilnShellOpts g_opt;
static int g_w, g_h;
static int g_audio;

static void present(void *ctx, const void *rgba8, int w, int h)
{
    (void)ctx;
    if (w != g_w || h != g_h) { web_open(w, h, g_opt.title); g_w = w; g_h = h; }
    web_blit(rgba8, w, h);
    kiln_shell_presented();
}

static void vsync(void *ctx)
{
    (void)ctx;

    unsigned char keys[KILN_KEY_COUNT];
    memset(keys, 0, sizeof keys);
    web_keys(keys, (int)sizeof keys);

    KilnShellPad p;
    kiln_shell_pad_from_keys(keys, &p);

    const int gp = web_gamepad();
    if (gp & 1) {
        p.a       |= (gp >> 1)  & 1;  p.b       |= (gp >> 2)  & 1;
        p.z       |= (gp >> 3)  & 1;  p.l       |= (gp >> 4)  & 1;
        p.r       |= (gp >> 5)  & 1;  p.start   |= (gp >> 6)  & 1;
        p.d_up    |= (gp >> 7)  & 1;  p.d_down  |= (gp >> 8)  & 1;
        p.d_left  |= (gp >> 9)  & 1;  p.d_right |= (gp >> 10) & 1;
        p.c_up    |= (gp >> 11) & 1;  p.c_down  |= (gp >> 12) & 1;
        p.c_left  |= (gp >> 13) & 1;  p.c_right |= (gp >> 14) & 1;
        const int8_t sx = (int8_t)((gp >> 16) & 0xFF);
        const int8_t sy = (int8_t)((gp >> 24) & 0xFF);
        if (sx) p.stick_x = sx;
        if (sy) p.stick_y = sy;
    }
    kiln_shell_pad(&p);

    /* The yield. emscripten_sleep returns control to the browser's event loop
     * — which is what lets the key listeners above ever run — and comes back
     * here when the timer fires. Always at least 1 ms: sleeping 0 in a tight
     * loop starves rendering on some engines. */
    const uint32_t wait = kiln_shell_pace((uint32_t)emscripten_get_now(), g_opt.fps);
    emscripten_sleep(wait ? wait : 1);
}

static int audio_free(void *ctx)
{
    (void)ctx;
    if (g_opt.mute) return 0;
    if (!g_audio) {
        const int freq = audio_get_frequency();
        if (freq <= 0) return 0;
        g_audio = web_audio_open(freq);
        if (!g_audio) { g_opt.mute = 1; return 0; }
    }
    /* Keep about 80 ms scheduled. Less and a slow frame is a dropout; more
     * and a sound effect lands noticeably after the thing that caused it. */
    return web_audio_queued_ms() < 80;
}

static void audio_submit(void *ctx, const short *stereo, int nsamples)
{
    (void)ctx;
    if (!g_audio) return;
    web_audio_push(stereo, nsamples, audio_get_frequency());
}

/* The game is compiled with -Dmain=kiln_game_main; this file is compiled in
 * the same command, so the macro reaches here too and would rename the
 * launcher's own entry point along with the game's. */
#undef main

int main(int argc, char **argv)
{
    const int r = kiln_shell_args(argc, argv, &g_opt);
    if (r) return r < 0 ? 2 : 0;
    if (!g_opt.dfs) g_opt.dfs = "/assets";   /* where --preload-file lands it */
    kiln_shell_env(&g_opt);

    web_input_init();

    const KilnHostHooks hooks = {
        .present      = present,
        .vsync        = vsync,
        .audio_free   = audio_free,
        .audio_submit = audio_submit,
        .ctx          = NULL,
    };
    kiln_host_set_hooks(&hooks);
    kiln_shell_pace_reset((uint32_t)emscripten_get_now());

    return kiln_game_main();
}
