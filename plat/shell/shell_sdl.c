/* SPDX-License-Identifier: MIT
 *
 * shell_sdl.c — the native launcher: a window, a pad, a speaker.
 *
 * SDL2 and not GLFW/X11/Wayland because this has to be the same file on
 * x86_64, aarch64 and riscv64, on Linux and macOS, and because it needs a
 * gamepad and an audio device as well as a surface — SDL is the only
 * dependency that covers all three and is packaged everywhere.
 *
 * There is no OpenGL here and that is the point. plat/host's software
 * rasteriser draws the frame; SDL is handed the finished RGBA8888 buffer as a
 * streaming texture and scales it. So the pixels in this window are the same
 * pixels nix/checks/ compares against a reference, and the window is evidence
 * about the gated renderer rather than a second renderer that happens to look
 * similar. It also means every architecture and the browser show identical
 * output, which is what let the wasm32 gate share the x86_64 reference PNGs.
 *
 * Cost: a 320x240 software rasteriser and one texture upload per frame. That
 * is nothing on any machine this runs on, and it is the console's fill rate
 * that is scarce, not the host's — see CLAUDE.md, "The host build cannot see
 * fill rate".
 */
#include "kiln_shell.h"

#include <libdragon.h>
#include <kiln_host.h>

#include <SDL2/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SDL_Window      *win;
    SDL_Renderer    *ren;
    SDL_Texture     *tex;
    SDL_GameController *pad;
    int              tw, th;
    int              quit;
    KilnShellOpts    opt;

    /* audio: one ring of stereo frames, filled by the mixer, drained by the
     * device callback. Sized to a quarter second, which is well past any
     * frame hitch and still under the latency a player would notice. */
    SDL_AudioDeviceID dev;
    short           *ring;
    int              ring_frames;   /* capacity, in stereo frames */
    int              head, tail;    /* head writes, tail reads    */
    SDL_atomic_t     used;          /* stereo frames queued       */

    unsigned char    keys[KILN_KEY_COUNT];
} Shell;

static Shell g;

/* ── the surface ───────────────────────────────────────────────────── */

static void window_open(int w, int h)
{
    int scale = g.opt.scale;
    if (scale <= 0) {
        /* Pick the largest integer scale that leaves the window comfortably
         * inside the display. Integer, always: a 320x240 framebuffer at a
         * fractional scale is a blurred one, and this frame's whole value is
         * that it is the same frame the console draws. */
        SDL_DisplayMode dm;
        scale = 3;
        if (SDL_GetCurrentDisplayMode(0, &dm) == 0) {
            const int sx = (dm.w * 4 / 5) / (w ? w : 1);
            const int sy = (dm.h * 4 / 5) / (h ? h : 1);
            scale = sx < sy ? sx : sy;
            if (scale < 1) scale = 1;
            if (scale > 8) scale = 8;
        }
    }

    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI;
    if (g.opt.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    g.win = SDL_CreateWindow(g.opt.title,
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             w * scale, h * scale, flags);
    if (!g.win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); exit(1); }

    g.ren = SDL_CreateRenderer(g.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g.ren) g.ren = SDL_CreateRenderer(g.win, -1, SDL_RENDERER_SOFTWARE);
    if (!g.ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); exit(1); }

    SDL_RenderSetLogicalSize(g.ren, w, h);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");   /* nearest */

    g.tex = SDL_CreateTexture(g.ren, SDL_PIXELFORMAT_ABGR8888,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!g.tex) { fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError()); exit(1); }
    g.tw = w; g.th = h;
}

static void present(void *ctx, const void *rgba8, int w, int h)
{
    (void)ctx;
    if (!g.win || w != g.tw || h != g.th) {
        if (g.tex) { SDL_DestroyTexture(g.tex); g.tex = NULL; }
        if (g.win) { SDL_DestroyRenderer(g.ren); SDL_DestroyWindow(g.win); g.win = NULL; }
        window_open(w, h);
    }
    /* host_gfx.c's buffer is R,G,B,A in memory order. ABGR8888 is SDL's name
     * for exactly that on a little-endian host, and its byte-order name
     * SDL_PIXELFORMAT_RGBA32 resolves to the same thing on a big-endian one. */
    SDL_UpdateTexture(g.tex, NULL, rgba8, w * 4);
    SDL_RenderClear(g.ren);
    SDL_RenderCopy(g.ren, g.tex, NULL, NULL);
    SDL_RenderPresent(g.ren);
    kiln_shell_presented();
}

/* ── input ─────────────────────────────────────────────────────────── */

static int key_index(SDL_Keycode k)
{
    switch (k) {
    case SDLK_w: return KILN_KEY_UP;      case SDLK_s: return KILN_KEY_DOWN;
    case SDLK_a: return KILN_KEY_LEFT;    case SDLK_d: return KILN_KEY_RIGHT;
    case SDLK_UP: return KILN_KEY_DUP;    case SDLK_DOWN: return KILN_KEY_DDOWN;
    case SDLK_LEFT: return KILN_KEY_DLEFT; case SDLK_RIGHT: return KILN_KEY_DRIGHT;
    case SDLK_SPACE: case SDLK_PERIOD: return KILN_KEY_A;
    case SDLK_COMMA: return KILN_KEY_B;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return KILN_KEY_Z;
    case SDLK_q: return KILN_KEY_L;       case SDLK_e: return KILN_KEY_R;
    case SDLK_RETURN: return KILN_KEY_START;
    case SDLK_i: return KILN_KEY_CUP;     case SDLK_k: return KILN_KEY_CDOWN;
    case SDLK_j: return KILN_KEY_CLEFT;   case SDLK_l: return KILN_KEY_CRIGHT;
    default: return -1;
    }
}

static int8_t axis_to_stick(Sint16 v)
{
    /* SDL's ±32767 into the console's ±90, with a small dead centre so a worn
     * stick does not creep. kiln_input applies the real (radial) deadzone; a
     * second one here would compound with it, so this is only enough to stop
     * hardware noise. */
    if (v > -3000 && v < 3000) return 0;
    int s = (v * JOYPAD_RANGE_N64_STICK_MAX) / 32767;
    if (s >  JOYPAD_RANGE_N64_STICK_MAX) s =  JOYPAD_RANGE_N64_STICK_MAX;
    if (s < -JOYPAD_RANGE_N64_STICK_MAX) s = -JOYPAD_RANGE_N64_STICK_MAX;
    return (int8_t)s;
}

static void pump(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: g.quit = 1; break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) g.quit = 1;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11 && g.win) {
                const Uint32 f = SDL_GetWindowFlags(g.win);
                SDL_SetWindowFullscreen(g.win,
                    (f & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
            const int k = key_index(e.key.keysym.sym);
            if (k >= 0) g.keys[k] = (e.type == SDL_KEYDOWN);
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
            if (!g.pad) g.pad = SDL_GameControllerOpen(e.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g.pad) { SDL_GameControllerClose(g.pad); g.pad = NULL; }
            break;
        default: break;
        }
    }

    KilnShellPad p;
    kiln_shell_pad_from_keys(g.keys, &p);

    if (g.pad) {
        /* The gamepad ORs into the keyboard rather than replacing it: a
         * launcher that switched exclusively to whichever was touched last
         * makes a two-handed test (stick on the pad, a debug key on the
         * keyboard) impossible. */
        const int8_t ax = axis_to_stick(SDL_GameControllerGetAxis(g.pad, SDL_CONTROLLER_AXIS_LEFTX));
        const int8_t ay = axis_to_stick(SDL_GameControllerGetAxis(g.pad, SDL_CONTROLLER_AXIS_LEFTY));
        if (ax) p.stick_x = ax;
        if (ay) p.stick_y = (int8_t)-ay;      /* SDL is Y-down, the N64 is not */

        #define BTN(b) SDL_GameControllerGetButton(g.pad, (b))
        p.a     |= BTN(SDL_CONTROLLER_BUTTON_A);
        p.b     |= BTN(SDL_CONTROLLER_BUTTON_X) | BTN(SDL_CONTROLLER_BUTTON_B);
        p.start |= BTN(SDL_CONTROLLER_BUTTON_START);
        p.l     |= BTN(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        p.r     |= BTN(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        p.d_up    |= BTN(SDL_CONTROLLER_BUTTON_DPAD_UP);
        p.d_down  |= BTN(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        p.d_left  |= BTN(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        p.d_right |= BTN(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        #undef BTN
        /* Z is the left trigger: it is the N64's index-finger button and a
         * shoulder press is the wrong shape for how it is used. */
        p.z |= SDL_GameControllerGetAxis(g.pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000;

        const Sint16 cx = SDL_GameControllerGetAxis(g.pad, SDL_CONTROLLER_AXIS_RIGHTX);
        const Sint16 cy = SDL_GameControllerGetAxis(g.pad, SDL_CONTROLLER_AXIS_RIGHTY);
        p.c_left  |= cx < -12000;  p.c_right |= cx > 12000;
        p.c_up    |= cy < -12000;  p.c_down  |= cy > 12000;
    }

    kiln_shell_pad(&p);
}

static void vsync(void *ctx)
{
    (void)ctx;
    pump();
    if (g.quit) {
        /* The game loop is a for(;;) in the ROM's own main and has no exit —
         * correctly, because on console there is nothing to exit to. So the
         * launcher leaves from here. SDL_Quit runs from atexit. */
        exit(0);
    }
    const uint32_t wait = kiln_shell_pace(SDL_GetTicks(), g.opt.fps);
    if (wait) SDL_Delay(wait);
}

/* ── audio ─────────────────────────────────────────────────────────── */

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    short *out = (short *)stream;
    int frames = len / (int)(2 * sizeof(short));
    const int have = SDL_AtomicGet(&g.used);
    const int take = frames < have ? frames : have;

    for (int i = 0; i < take; i++) {
        out[i * 2 + 0] = g.ring[g.tail * 2 + 0];
        out[i * 2 + 1] = g.ring[g.tail * 2 + 1];
        g.tail = (g.tail + 1) % g.ring_frames;
    }
    /* Underrun is silence, not the last buffer repeated: a repeat is a buzz
     * that sounds like a synthesis bug, and this is a scheduling one. */
    for (int i = take; i < frames; i++) { out[i * 2] = 0; out[i * 2 + 1] = 0; }
    SDL_AtomicAdd(&g.used, -take);
}

static void audio_open(void)
{
    const int freq = audio_get_frequency();
    if (freq <= 0) return;

    SDL_AudioSpec want, got;
    memset(&want, 0, sizeof want);
    want.freq     = freq;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 512;
    want.callback = audio_cb;

    g.ring_frames = freq / 4;
    g.ring = calloc((size_t)g.ring_frames * 2, sizeof(short));
    if (!g.ring) return;

    g.dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!g.dev) {
        fprintf(stderr, "kiln: no audio device (%s); running silent\n", SDL_GetError());
        free(g.ring); g.ring = NULL;
        return;
    }
    SDL_PauseAudioDevice(g.dev, 0);
}

static int audio_free(void *ctx)
{
    (void)ctx;
    if (!g.dev && !g.opt.mute) audio_open();
    if (!g.dev) {
        /* No device, and the game still has to make progress. One buffer per
         * frame is what a 60 Hz device would take, and the mixer's channel
         * bookkeeping — which is what kiln_audio is almost entirely made of —
         * runs exactly as it would with a speaker attached. */
        static int credit;
        if (++credit >= 4) { credit = 0; return 0; }
        return 1;
    }
    const int room = g.ring_frames - SDL_AtomicGet(&g.used);
    return room / audio_get_buffer_length();
}

static void audio_submit(void *ctx, const short *stereo, int nsamples)
{
    (void)ctx;
    if (!g.dev || !g.ring) return;
    SDL_LockAudioDevice(g.dev);
    for (int i = 0; i < nsamples; i++) {
        if (SDL_AtomicGet(&g.used) >= g.ring_frames) break;
        g.ring[g.head * 2 + 0] = stereo[i * 2 + 0];
        g.ring[g.head * 2 + 1] = stereo[i * 2 + 1];
        g.head = (g.head + 1) % g.ring_frames;
        SDL_AtomicAdd(&g.used, 1);
    }
    SDL_UnlockAudioDevice(g.dev);
}

/* ── entry ─────────────────────────────────────────────────────────── */

static void shutdown_sdl(void) { SDL_Quit(); }

/* The game is compiled with -Dmain=kiln_game_main; this file is compiled in
 * the same command, so the macro reaches here too and would rename the
 * launcher's own entry point along with the game's. */
#undef main

int main(int argc, char **argv)
{
    const int r = kiln_shell_args(argc, argv, &g.opt);
    if (r) return r < 0 ? 2 : 0;
    kiln_shell_env(&g.opt);

    Uint32 sub = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER;
    if (!g.opt.mute) sub |= SDL_INIT_AUDIO;
    if (SDL_Init(sub) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    atexit(shutdown_sdl);

    const KilnHostHooks hooks = {
        .present      = present,
        .vsync        = vsync,
        .audio_free   = g.opt.mute ? NULL : audio_free,
        .audio_submit = g.opt.mute ? NULL : audio_submit,
        .ctx          = NULL,
    };
    kiln_host_set_hooks(&hooks);
    kiln_shell_pace_reset(SDL_GetTicks());

    return kiln_game_main();
}
