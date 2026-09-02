/* SPDX-License-Identifier: MIT
 *
 * kiln-wav64-check.c — a .wav64 built by this repo's own pipeline decodes,
 * and the mixer turns it into sound.
 *
 * Two claims, and the second is the one that has been wrong before in this
 * project's history in other guises: it is not enough that a file parses. A
 * decoder that returns the right number of samples and fills them with zeroes
 * passes every structural assertion and is silent, and silence is the single
 * hardest audio failure to attribute — it looks identical to a missing asset,
 * a stopped channel, a zero volume, or a wrong partition. So this measures
 * the samples.
 */
#include <libdragon.h>
#include <kiln/kiln_audio.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double rms_of(const int16_t *pcm, int n)
{
    double acc = 0;
    for (int i = 0; i < n; i++) acc += (double)pcm[i] * (double)pcm[i];
    return n ? sqrt(acc / n) / 32768.0 : 0.0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <rom:/path.wav64>\n", argv[0]); return 2; }

    dfs_init(0);
    audio_init(32000, 4);
    mixer_init(8);

    wav64_t w;
    wav64_open(&w, argv[1]);

    const KilnHostAudioCounters *c = kiln_host_audio_counters();
    printf("wav64: opens %u missing %u\n", c->wav_opens, c->wav_missing);
    if (c->wav_missing) { fprintf(stderr, "FAILED: the file did not decode\n"); return 1; }

    printf("waveform: %d ch, %d bits, %d Hz, %d samples, loop %d\n",
           w.wave.channels, w.wave.bits, w.wave.frequency, w.wave.len, w.wave.loop_len);
    if (w.wave.len <= 0)     { fprintf(stderr, "FAILED: no samples\n"); return 1; }
    if (w.wave.frequency <= 0) { fprintf(stderr, "FAILED: no sample rate\n"); return 1; }

    /* The decoded PCM, straight from the waveform, before any mixing. */
    const int16_t *pcm = NULL;
    int nsamp = 0;
    {
        /* wave.ctx is host_wav64.c's KilnHostWave. The check reaches into it
         * on purpose: the point is to measure what the DECODER produced, not
         * what survived the mixer's volume. */
        struct { int16_t *pcm; int samples, channels, rate, loop_len; } *hw = w.wave.ctx;
        if (!hw) { fprintf(stderr, "FAILED: no decoded PCM attached\n"); return 1; }
        pcm = hw->pcm; nsamp = hw->samples * hw->channels;
    }
    const double src_rms = rms_of(pcm, nsamp);
    printf("decoded rms: %.4f full-scale over %d samples\n", src_rms, nsamp);
    if (src_rms < 0.001) {
        fprintf(stderr, "FAILED: the decoder produced %d samples of near-silence.\n"
                        "        A decoder that returns the right COUNT and the wrong\n"
                        "        CONTENT passes every structural check there is.\n", nsamp);
        return 1;
    }

    /* Now through the real mixer, at full volume, dead centre. */
    mixer_ch_set_vol_pan(0, 1.0f, 0.5f);
    wav64_play(&w, 0);
    if (!mixer_ch_playing(0)) { fprintf(stderr, "FAILED: channel 0 is not playing\n"); return 1; }

    enum { N = 4096 };
    int16_t *out = calloc(N * 2, sizeof(int16_t));
    mixer_poll(out, N);
    const double mix_rms = rms_of(out, N * 2);
    printf("mixed rms:   %.4f full-scale over %d stereo frames\n", mix_rms, N);
    if (mix_rms < 0.0005) {
        fprintf(stderr, "FAILED: the decoder produced audio and the mixer did not.\n");
        return 1;
    }

    /* Pan hard left: the right channel must go quiet and the left must not.
     * kiln_sound computes pan from the listener basis, so a mixer that had
     * this backwards would put every positional sound on the wrong side —
     * which is audible, deniable, and exactly the sort of thing nobody files
     * a bug about. */
    mixer_ch_stop(0);
    wav64_play(&w, 0);
    mixer_ch_set_vol_pan(0, 1.0f, 0.0f);
    memset(out, 0, (size_t)N * 2 * sizeof(int16_t));
    mixer_poll(out, N);
    double l = 0, r = 0;
    for (int i = 0; i < N; i++) { l += fabs((double)out[i*2]); r += fabs((double)out[i*2+1]); }
    printf("pan 0.0:     left %.0f  right %.0f\n", l, r);
    if (!(l > 0 && r == 0)) {
        fprintf(stderr, "FAILED: pan 0.0 should be hard LEFT (libdragon's convention)\n");
        return 1;
    }

    free(out);
    wav64_close(&w);
    printf("wav64 decodes, mixes and pans\n");
    return 0;
}
