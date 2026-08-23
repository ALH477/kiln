// SPDX-License-Identifier: MIT
//
// Report Stage 1, end to end: a Faust instrument rendered offline at full
// quality on the host, VADPCM-encoded by audioconv64, packed into DragonFS,
// and played back through libdragon's RSP mixer.
//
// This is the path the feasibility report recommends for 80-90% of a game's
// audio, and the reason it is worth doing first: none of the VR4300's
// arithmetic limits apply here. The CPU cost is sample playback and mixing,
// which libdragon benchmarks at "< 3% CPU and < 10% RSP" for a 10-channel XM.
// The expensive thing on this console is *synthesis*, not playback.
//
// The live-synthesis counterpart is dsp/arch/libdragon_mixer.c.

#include <libdragon.h>

// Must match the sampleRate given to mkBakedInstrument. The AI derives its
// rate from a video-clock divider, so this is an N64-side decision — the M64
// resamples whatever the core produces to its fixed 48 kHz HDMI output.
#define SAMPLE_RATE 32000

// One channel is enough for a single one-shot. libdragon's mixer supports 32.
#define NUM_CHANNELS 1
#define CH_INSTRUMENT 0

int main(void)
{
    console_init();
    console_set_render_mode(RENDER_MANUAL);
    debug_init_isviewer();

    // DFS_DEFAULT_LOCATION makes dfs_init find the filesystem image via the
    // rompak table of contents rather than a hard-coded offset.
    dfs_init(DFS_DEFAULT_LOCATION);

    audio_init(SAMPLE_RATE, 4);
    mixer_init(NUM_CHANNELS);

    wav64_t inst;
    wav64_open(&inst, "rom:/ksvoice.wav64");
    wav64_set_loop(&inst, true);

    debugf("Kiln: playing ksvoice.wav64 at %d Hz\n", SAMPLE_RATE);
    wav64_play(&inst, CH_INSTRUMENT);

    while (1) {
        console_clear();
        printf("\n  Kiln - baked audio\n\n");
        printf("  ksvoice.wav64 (VADPCM)\n");
        printf("  %d Hz, mixer ch %d\n\n", SAMPLE_RATE, CH_INSTRUMENT);
        printf("  playing: %s\n", mixer_ch_playing(CH_INSTRUMENT) ? "yes" : "no");
        console_render();

        // Keep the audio buffers fed. mixer_poll renders directly into the
        // buffer the AI will DMA from, so this must be called often enough to
        // stay ahead of playback — audio_can_write() is the backpressure.
        while (audio_can_write()) {
            short *buf = audio_write_begin();
            mixer_poll(buf, audio_get_buffer_length());
            audio_write_end();
        }
    }
}
