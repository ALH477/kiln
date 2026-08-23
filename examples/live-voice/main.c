// SPDX-License-Identifier: MIT
//
// Live Faust voice + baked instrument comparison.
//
// This ROM proves the live synthesis path end-to-end: a Faust-generated
// MIPS object (ks-voice) is linked into the ROM, initialised, and rendered
// sample-by-sample on the VR4300, mixed with the RSP mixer's output.
//
// Controls:
//   A: trigger live KS voice (freq cycles 220→880 on each press)
//   B: play baked KS sample (VADPCM via RSP mixer)
//
// The live path and the baked path both originate from dsp/ks.dsp. This
// ROM is the A/B comparison point: the baked path is the golden reference
// (full double-precision render), the live path is what the VR4300 produces
// in real time at single precision.

#include <libdragon.h>
#include <kiln/kiln_audio.h>

// The Faust voice API, declared by dsp/arch/libdragon_mixer.c with -cn ksvoice.
// These are the mangled symbols from faust -cn ksvoice.
#define FAUST_NAME ksvoice
#define FAUST_CAT2(a, b) a##b
#define FAUST_CAT(a, b) FAUST_CAT2(a, b)
#define FAUST_VOICE_PREFIX faust_n64_ksvoice

void FAUST_CAT(FAUST_VOICE_PREFIX, _init)(int sample_rate);
float *FAUST_CAT(FAUST_VOICE_PREFIX, _param)(const char *label);
void FAUST_CAT(FAUST_VOICE_PREFIX, _set_gain)(float gain);
void FAUST_CAT(FAUST_VOICE_PREFIX, _render)(int16_t *out, int nframes, int accumulate);

#define SAMPLE_RATE 32000

// SFX channels: 1 for the baked instrument.
// Music channels: 0 (no tracker music in this demo).
#define CH_BAKED 0

// Scratch buffer for the live voice render. Allocated once, reused every frame.
// Size: audio_get_buffer_length() * 2 (stereo) * sizeof(int16_t).
// We allocate for the maximum possible buffer length at 32000 Hz.
#define MAX_BUF_LEN 1024
static int16_t scratch[MAX_BUF_LEN * 2];

int main(void)
{
    console_init();
    console_set_render_mode(RENDER_MANUAL);
    debug_init_isviewer();
    dfs_init(DFS_DEFAULT_LOCATION);

    // Initialise the engine audio layer.
    kiln_audio_init((KilnAudioConfig){
        .sample_rate = SAMPLE_RATE,
        .latency = 0.16f,
        .sfx_channels = 1,
        .music_channels = 0,
    });

    // Load the baked instrument (same .wav64 as examples/audio).
    int baked = kiln_sfx_load("rom:/ksvoice.wav64");
    if (baked < 0) {
        debugf("FAIL: could not load ksvoice.wav64\n");
        while (1) {}
    }

    // Initialise the live Faust voice.
    faust_n64_ksvoice_init(SAMPLE_RATE);
    faust_n64_ksvoice_set_gain(0.5f);

    // Cache parameter zone pointers — writing a parameter is then a single store.
    float *freq_zone = faust_n64_ksvoice_param("freq");
    float *gain_zone = faust_n64_ksvoice_param("gain");
    float *gate_zone = faust_n64_ksvoice_param("gate");

    if (freq_zone) *freq_zone = 220.0f;
    if (gain_zone) *gain_zone = 0.5f;
    if (gate_zone) *gate_zone = 0.0f;

    // Frequency cycle for the live voice.
    const float freqs[] = { 220.0f, 277.0f, 330.0f, 440.0f, 554.0f, 660.0f, 880.0f };
    int freq_idx = 0;

    while (1) {
        joypad_poll();
        joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);

        if (pressed.a) {
            // Trigger the live KS voice.
            float f = freqs[freq_idx % 7];
            freq_idx++;
            if (freq_zone) *freq_zone = f;
            if (gate_zone) *gate_zone = 1.0f;
            debugf("live: freq=%.1f\n", f);
        }

        if (pressed.b) {
            // Play the baked instrument for comparison.
            kiln_sfx_play(baked, CH_BAKED, 1);
            debugf("baked: ksvoice.wav64\n");
        }

        // Render the live voice into the audio buffer.
        // Strategy: get the AI buffer, run mixer_poll for the baked channels,
        // then overlay the live voice with accumulate=1.
        while (audio_can_write()) {
            short *buf = audio_write_begin();
            int n = audio_get_buffer_length();
            if (n > MAX_BUF_LEN) n = MAX_BUF_LEN;

            // RSP mixer: render baked channels into the AI buffer.
            mixer_poll(buf, n);

            // VR4300: render the live voice and add it on top.
            // Only if the gate was recently triggered — the KS voice
            // produces silence when gate=0 and the delay line has decayed.
            faust_n64_ksvoice_render(scratch, n, 0);
            for (int i = 0; i < n * 2; i++) {
                int32_t s = (int32_t)buf[i] + (int32_t)scratch[i];
                buf[i] = (s > 32767) ? 32767 : (s < -32768) ? -32768 : (int16_t)s;
            }

            audio_write_end();
        }

        // HUD
        console_clear();
        printf("\n  Kiln - live vs baked\n\n");
        printf("  A: live KS voice (freq=%.0f)\n", freq_zone ? *freq_zone : 0);
        printf("  B: baked ksvoice.wav64\n\n");
        printf("  baked playing: %s\n", kiln_sfx_playing(CH_BAKED) ? "yes" : "no");
        printf("  gate: %.0f\n", gate_zone ? *gate_zone : 0);
        console_render();

        // Release the gate after one frame — KS excites on the rising edge.
        if (gate_zone && *gate_zone > 0.0f) *gate_zone = 0.0f;
    }
}