// SPDX-License-Identifier: MIT
//
// XM64 tracker music playback example.
//
// Plays a simple XM file converted to .xm64 by mkMusic + audioconv64.
// The XM64 player uses libdragon's RSP mixer — libdragon benchmarks a
// 10-channel XM at "< 3% CPU and < 10% RSP", which is why report §5
// calls XM64 the pragmatic music engine for this target.
//
// Controls:
//   A: play / pause
//   B: stop
//   Up/Down: volume

#include <libdragon.h>
#include <kiln/kiln_audio.h>

#define SAMPLE_RATE 32000

int main(void)
{
    console_init();
    console_set_render_mode(RENDER_MANUAL);
    debug_init_isviewer();
    dfs_init(DFS_DEFAULT_LOCATION);

    // 0 SFX channels, 10 music channels (the XM uses 4).
    kiln_audio_init((KilnAudioConfig){
        .sample_rate = SAMPLE_RATE,
        .latency = 0.16f,
        .sfx_channels = 0,
        .music_channels = 10,
    });

    int music = kiln_music_load("rom:/music/test.xm64");
    if (music < 0) {
        debugf("FAIL: could not load test.xm64\n");
        while (1) {}
    }

    debugf("Kiln: loaded test.xm64 (%d channels)\n",
           kiln_music_num_channels(music));

    float vol = 0.7f;
    int playing = 0;

    while (1) {
        joypad_poll();
        joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        joypad_buttons_t held = joypad_get_buttons_held(JOYPAD_PORT_1);

        if (pressed.a) {
            if (kiln_music_playing(music)) {
                kiln_music_stop(music);
                playing = 0;
            } else {
                kiln_music_play(music);
                kiln_music_set_volume(music, vol);
                playing = 1;
            }
        }

        if (pressed.b) {
            kiln_music_stop(music);
            playing = 0;
        }

        if (held.d_up) {
            vol += 0.05f;
            if (vol > 1.0f) vol = 1.0f;
            kiln_music_set_volume(music, vol);
        }
        if (held.d_down) {
            vol -= 0.05f;
            if (vol < 0.0f) vol = 0.0f;
            kiln_music_set_volume(music, vol);
        }

        kiln_audio_update();

        console_clear();
        printf("\n  Kiln - XM64 music\n\n");
        printf("  test.xm64 (%d ch)\n", kiln_music_num_channels(music));
        printf("  %d Hz\n\n", SAMPLE_RATE);
        printf("  A: %s\n", playing ? "stop" : "play");
        printf("  B: stop\n");
        printf("  Up/Down: vol %.0f%%\n\n", vol * 100);
        printf("  playing: %s\n", kiln_music_playing(music) ? "yes" : "no");
        console_render();
    }
}