/* SPDX-License-Identifier: MIT
 *
 * host_audio.c — the RSP mixer's channel arithmetic, and now its samples.
 *
 * Nearly all of kiln_audio is bookkeeping: a 32-channel budget partitioned
 * into SFX and music ranges, priority-based voice stealing, room crossfades.
 * That arithmetic is either right or produces a silence nobody can explain,
 * and it needs no PCM to be checkable — which is why this file tracked
 * channel state exactly and output silence for as long as the host tier was
 * only a gate.
 *
 * It is no longer only a gate. mixer_poll now mixes for real: nearest-
 * neighbour resampling, per-channel volume and pan, loop_len counted back
 * from the END of the waveform (libdragon's convention, not the obvious one),
 * summed and clamped. host_wav64.c turns a .wav64 into the PCM it reads.
 *
 * Nearest and not linear interpolation on purpose: the RSP steps a
 * fixed-point cursor and takes the sample it lands on, and a host that
 * interpolated would sound BETTER than the console — the same mistake as a
 * host that rendered more precisely than it. See host_t3d.c honouring the
 * s16.16 matrix quantisation for the visual half of that argument.
 *
 * What is still bookkeeping, and says so at the point of use: XM64 and YM64
 * tracker playback. libdragon's player is not separable from the RSP mixer
 * the way the VADPCM codec is.
 */
#include <libdragon.h>
#include <kiln_host.h>
#include "host_internal.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#define MAX_CH 32

static struct {
    int   playing;
    float lvol, rvol, freq;
    float max_freq;        /* mixer_ch_set_limits; the output rate by default */
    int   sub;             /* CH_FLAGS_STEREO_SUB: right half of ch-1's stereo */
    const waveform_t *wave;
    double pos;            /* play cursor, in source samples */
} g_ch[MAX_CH];
static int g_nch;
static int g_freq;
static float g_master = 1.0f;
static short g_buf[4096];
static KilnHostAudioCounters g_c;
static int g_said_silent;

const KilnHostAudioCounters *kiln_host_audio_counters(void)
{
    g_c.channels = g_nch;
    g_c.frequency = g_freq;
    g_c.channels_playing = 0;
    for (int i = 0; i < g_nch; i++) if (g_ch[i].playing) g_c.channels_playing++;
    return &g_c;
}

/* ── the output device, which does not exist ───────────────────────────
 * audio_can_write used to `return 1`, and that is not a stub, it is a hang:
 * kiln_audio_update is `while (audio_can_write()) { ... }`, draining until the
 * device says full. A device that is never full never lets the frame end, so
 * any host build of a real game loop — as opposed to a check, which never
 * calls it — spins forever on the first frame. Nothing caught this because
 * nothing had run a game loop on the host.
 *
 * So the host models a real device's occupancy, and credits it on PRESENTED
 * FRAMES rather than on wall time. That is the only clock a deterministic
 * check is allowed: two runs of the same ROM push the same number of buffers
 * in the same order, on any machine, at any speed. A launcher that has an
 * actual speaker installs audio_free/audio_submit and the credit model steps
 * aside for the device's real occupancy. */
#define BUFLEN 512
#define NBUF   4

static int g_credit;   /* samples the device could still accept */

void audio_init(const int frequency, float latency)
{
    (void)latency;
    assertf(frequency > 0, "audio_init: frequency %d", frequency);
    g_freq = frequency;
    g_credit = NBUF * BUFLEN;   /* an empty device: prefill it */
    memset(&g_c, 0, sizeof g_c);
}
void audio_close(void) { g_freq = 0; g_credit = 0; }

void kiln_host_audio_frame(void)
{
    const KilnHostHooks *h = kiln_host_hooks();
    if (h->audio_free) return;          /* a real device keeps its own count */
    if (g_freq <= 0) return;
    g_credit += g_freq / 60;            /* one frame of samples consumed */
    if (g_credit > NBUF * BUFLEN) g_credit = NBUF * BUFLEN;
}

int audio_can_write(void)
{
    const KilnHostHooks *h = kiln_host_hooks();
    if (h->audio_free) return h->audio_free(h->ctx) > 0;
    return g_credit >= BUFLEN;
}
int  audio_get_frequency(void) { return g_freq; }
int  audio_get_buffer_length(void) { return BUFLEN; }
short *audio_write_begin(void) { return g_buf; }
void  audio_write_end(void)
{
    const KilnHostHooks *h = kiln_host_hooks();
    if (h->audio_submit) h->audio_submit(h->ctx, g_buf, BUFLEN);
    else                 g_credit -= BUFLEN;
}

void mixer_init(int num_channels)
{
    /* The console's hard limit. kiln_audio's default partition is 16 SFX + 10
     * music = 26, and the header explains why the sum matters; a host that
     * allowed 40 would let a partition through that cannot exist. */
    assertf(num_channels > 0 && num_channels <= MAX_CH,
            "mixer_init(%d): the RSP mixer has %d channels", num_channels, MAX_CH);
    g_nch = num_channels;
    memset(g_ch, 0, sizeof g_ch);
    for (int i = 0; i < MAX_CH; i++) g_ch[i].max_freq = (float)(g_freq > 0 ? g_freq : 32000);
}

void mixer_close(void) { g_nch = 0; }
void mixer_set_vol(float vol) { g_master = vol; }

static void check_ch(int ch, const char *who)
{
    assertf(ch >= 0 && ch < g_nch,
            "%s: channel %d is outside the %d the mixer was initialised with",
            who, ch, g_nch);
}

/* libdragon's default limit is the output rate, and mixer_ch_set_freq ASSERTS
 * above it (mixer.c, with a 1% rounding margin). The host did not, so a game
 * pitching a 32 kHz sample up an octave ran here and died on the console. */
void mixer_ch_set_limits(int ch, int max_bits, float max_frequency, int max_buf_sz)
{
    (void)max_bits; (void)max_buf_sz;
    check_ch(ch, "mixer_ch_set_limits");
    g_ch[ch].max_freq = max_frequency > 0.0f ? max_frequency
                                             : (float)(g_freq > 0 ? g_freq : 32000);
}

void mixer_ch_play(int ch, waveform_t *wave)
{
    check_ch(ch, "mixer_ch_play");
    /* libdragon marks ch+1 the SECONDARY of a stereo waveform and asserts on
     * play / set_vol / set_freq through it. The host mixes stereo on one
     * channel and used to let all three through, so a game that played onto a
     * secondary ran here and died on the console (kiln_audio's allocator did
     * exactly that; see kiln-audio-check.c). */
    assertf(!g_ch[ch].sub, "mixer_ch_play: cannot call on secondary stereo channel %d", ch);
    if (wave && wave->channels == 2) {
        assertf(ch != g_nch - 1, "cannot configure last channel (%d) as stereo", ch);
        assertf(!mixer_ch_playing(ch + 1) || g_ch[ch + 1].sub,
                "cannot play stereo waveform on channel %d because channel %d is active", ch, ch + 1);
        g_ch[ch + 1].sub = 1;
    } else if (ch != g_nch - 1) {
        g_ch[ch + 1].sub = 0;
    }
    /* Reset the playback frequency whenever the WAVEFORM changes, which is
     * what libdragon does — mixer.c calls mixer_ch_set_freq(ch,
     * wave->frequency) unconditionally inside its "configure the waveform"
     * branch, and skips it only when the same waveform is replayed on the
     * same channel.
     *
     * The first version of this only seeded a frequency that was still zero,
     * which made pitch STICKY: a game that pitch-shifts with
     * kiln_sfx_play_ex and then lets kiln_sfx_play steal that channel plays
     * the next sound at the previous sound's pitch, and two assets with
     * different sample rates sharing an SFX channel both resample at
     * whichever landed first. That is the pitch-and-time-drift class
     * mkN64Rom's audioRate cross-check exists to catch, arriving silently
     * through the back door. */
    /* libdragon does this through mixer_ch_set_freq, so the limit assert
     * applies to the asset's own rate too: a 44.1 kHz wav64 on a 32 kHz
     * output dies here on the console unless the channel's limit is raised. */
    if (g_ch[ch].wave != wave && wave) {
        assertf((float)wave->frequency <= g_ch[ch].max_freq * 1.01f,
                "frequency %.1f exceeds configured limit %.1f on channel %d; use "
                "mixer_ch_set_limit to change the limit for this channel",
                (float)wave->frequency, g_ch[ch].max_freq, ch);
        g_ch[ch].freq = (float)wave->frequency;
    }
    g_ch[ch].playing = 1;
    g_ch[ch].wave = wave;
    g_ch[ch].pos = 0.0;
    g_c.ch_plays++;
}
void mixer_ch_set_vol(int ch, float lvol, float rvol)
{
    check_ch(ch, "mixer_ch_set_vol");
    assertf(!g_ch[ch].sub, "mixer_ch_set_vol: cannot call on secondary stereo channel %d", ch);
    g_ch[ch].lvol = lvol; g_ch[ch].rvol = rvol;
}
void mixer_ch_set_vol_pan(int ch, float vol, float pan)
{
    /* libdragon's own mapping: pan 0 is hard left, 1 hard right. kiln_sound
     * computes pan from the listener basis, so getting this backwards would
     * put every positional sound on the wrong side. */
    check_ch(ch, "mixer_ch_set_vol_pan");
    assertf(!g_ch[ch].sub, "mixer_ch_set_vol: cannot call on secondary stereo channel %d", ch);
    g_ch[ch].lvol = vol * (1.0f - pan);
    g_ch[ch].rvol = vol * pan;
}
void mixer_ch_set_freq(int ch, float frequency)
{
    check_ch(ch, "mixer_ch_set_freq");
    assertf(!g_ch[ch].sub, "cannot call on secondary stereo channel %d", ch);
    assertf(frequency >= 0, "cannot set negative frequency on channel %d: %f", ch, frequency);
    assertf(frequency <= g_ch[ch].max_freq * 1.01f,
            "frequency %.1f exceeds configured limit %.1f on channel %d; use "
            "mixer_ch_set_limit to change the limit for this channel",
            frequency, g_ch[ch].max_freq, ch);
    g_ch[ch].freq = frequency;
}
void mixer_ch_stop(int ch)
{
    check_ch(ch, "mixer_ch_stop");
    /* Stopping a stereo owner releases its secondary (mixer.c does the same). */
    if (g_ch[ch].playing && g_ch[ch].wave && g_ch[ch].wave->channels == 2 && ch + 1 < MAX_CH)
        g_ch[ch + 1].sub = 0;
    g_ch[ch].playing = 0;
    g_ch[ch].wave = NULL;
    g_c.ch_stops++;
}
/* Position in source samples, which is what the host cursor already counts.
 * Both assert on a secondary, as mixer.c does. */
void mixer_ch_set_pos(int ch, double pos)
{
    check_ch(ch, "mixer_ch_set_pos");
    assertf(!g_ch[ch].sub, "mixer_ch_set_pos: cannot call on secondary stereo channel %d", ch);
    g_ch[ch].pos = pos;
}
double mixer_ch_get_pos(int ch)
{
    check_ch(ch, "mixer_ch_get_pos");
    assertf(!g_ch[ch].sub, "mixer_ch_get_pos: cannot call on secondary stereo channel %d", ch);
    return g_ch[ch].pos;
}
waveform_t *mixer_ch_playing_waveform(int ch)
{
    if (ch < 0 || ch >= g_nch) return NULL;
    if (g_ch[ch].sub && ch > 0) ch--;
    return g_ch[ch].playing ? (waveform_t *)g_ch[ch].wave : NULL;
}

bool mixer_ch_playing(int ch)
{
    if (ch < 0 || ch >= g_nch) return false;
    /* A secondary answers for its owner, as mixer_ch_playing_waveform does. */
    if (g_ch[ch].sub && ch > 0) return g_ch[ch - 1].playing != 0;
    return g_ch[ch].playing != 0;
}

/* ── the mix ───────────────────────────────────────────────────────────
 * Nearest-neighbour resampling, not linear interpolation, and that is a
 * deliberate choice rather than a shortcut: the RSP mixer resamples by
 * stepping a fixed-point cursor and taking the sample it lands on, and a host
 * that interpolated would sound better than the console. Everything else in
 * plat/host is built to predict what the console does — host_t3d.c honours
 * the s16.16 matrix quantisation for exactly this reason — and a nicer
 * host mixer would make the host stop being evidence about the ROM.
 *
 * Channels whose waveform never decoded (a Huffman VADPCM, an Opus file) have
 * wave->ctx == NULL and contribute nothing. They still occupy their channel
 * and still report as playing, because that is what kiln_audio's partition
 * and voice-stealing arithmetic is entitled to see. */
void mixer_poll(int16_t *out, int nsamples)
{
    g_c.polls++;
    g_c.samples_requested += (uint32_t)(nsamples > 0 ? nsamples : 0);
    if (!out || nsamples <= 0) return;

    const int rate = g_freq > 0 ? g_freq : 32000;

    /* Grow-only, not per-call. This runs several times per frame for the life
     * of the process, so an allocator in the audio path is a needless
     * recurring cost — the console's mixer allocates nothing per poll either.
     *
     * Grow-ONLY and not a fixed BUFLEN array, which is what this was for
     * about ten minutes: mixer_poll's contract is libdragon's, and libdragon
     * takes any length. nix/checks/kiln-wav64-check.c polls 4096 against a
     * BUFLEN of 512 precisely because a mix is worth measuring over more than
     * one device buffer, and the fixed array asserted on its own check. */
    static int32_t *acc;
    static int      acc_cap;
    if (nsamples > acc_cap) {
        int32_t *grown = realloc(acc, (size_t)nsamples * 2 * sizeof(int32_t));
        if (!grown) { memset(out, 0, (size_t)nsamples * 2 * sizeof(int16_t)); return; }
        acc = grown;
        acc_cap = nsamples;
    }
    memset(acc, 0, (size_t)nsamples * 2 * sizeof(int32_t));

    int audible = 0;
    for (int c = 0; c < g_nch; c++) {
        if (!g_ch[c].playing || !g_ch[c].wave) continue;
        const KilnHostWave *w = (const KilnHostWave *)g_ch[c].wave->ctx;
        if (!w || !w->pcm) continue;
        audible++;

        const double step = (g_ch[c].freq > 0.0f ? (double)g_ch[c].freq : (double)w->rate)
                          / (double)rate;
        const float lv = g_ch[c].lvol * g_master;
        const float rv = g_ch[c].rvol * g_master;

        for (int i = 0; i < nsamples; i++) {
            long idx = (long)g_ch[c].pos;
            if (idx >= w->samples) {
                /* loop_len counts back from the END of the waveform, which is
                 * libdragon's convention and not the obvious one. */
                if (w->loop_len > 0 && w->loop_len <= w->samples) {
                    const long start = w->samples - w->loop_len;
                    g_ch[c].pos = start + fmod(g_ch[c].pos - w->samples, (double)w->loop_len);
                    idx = (long)g_ch[c].pos;
                } else {
                    /* A one-shot's end is a mixer_ch_stop on the console
                     * (mixer_update_loops), which releases a secondary. */
                    if (w->channels == 2 && c + 1 < MAX_CH) g_ch[c + 1].sub = 0;
                    g_ch[c].playing = 0;
                    g_ch[c].wave = NULL;
                    break;
                }
            }
            const int16_t *src = w->pcm + (size_t)idx * w->channels;
            const int32_t l = src[0];
            const int32_t r = w->channels == 2 ? src[1] : l;
            acc[i * 2 + 0] += (int32_t)(l * lv);
            acc[i * 2 + 1] += (int32_t)(r * rv);
            g_ch[c].pos += step;
        }
    }

    /* Clamp rather than wrap. A wrapped sum is a loud click and sounds like a
     * synthesis fault; a clamped one sounds like what it is, too many voices
     * at once — and mkBakedInstrument already gates for a render that runs
     * hot, so a game that clips here is telling you something true. */
    for (int i = 0; i < nsamples * 2; i++) {
        int32_t v = acc[i];
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        out[i] = (int16_t)v;
    }

    if (!audible && !g_said_silent) {
        g_said_silent = 1;
        debugf("host audio: nothing decodable is playing, so mixer_poll is "
               "producing silence. Channel state is still exact — see "
               "kiln_host_audio_counters.\n");
    }
}
void mixer_try_play(void) { }

void wav64_open(wav64_t *wav, const char *fn)
{
    assertf(wav != NULL, "wav64_open: NULL wav64_t");
    memset(wav, 0, sizeof *wav);
    g_c.wav_opens++;

    /* Through the host VFS, so a missing sound is a missing file rather than a
     * silence. This is the failure this project has actually had — twelve sfx
     * paths a game's flake never built — and it is exactly the class the DFS
     * gate was added for. */
    char err[128];
    KilnHostWave *w = kiln_host_wave_load(fn, err, sizeof err);
    if (!w) {
        g_c.wav_missing++;
        debugf("wav64_open: '%s': %s\n", fn, err);
        return;
    }

    wav->wave.channels  = w->channels;
    wav->wave.bits      = 16;
    wav->wave.frequency = w->rate;
    wav->wave.len       = w->samples;
    wav->wave.loop_len  = w->loop_len;
    wav->wave.ctx       = w;
    wav->st             = w;
}

void wav64_play(wav64_t *wav, int ch)
{
    assertf(wav != NULL, "wav64_play: NULL");
    mixer_ch_play(ch, &wav->wave);
}
void wav64_close(wav64_t *wav)
{
    if (!wav) return;
    /* Stop anything still pointing at the waveform first: freeing samples out
     * from under a playing channel is a use-after-free that presents as a
     * burst of noise, which reads as a decoder bug. */
    for (int i = 0; i < g_nch; i++)
        if (g_ch[i].wave == &wav->wave) { g_ch[i].playing = 0; g_ch[i].wave = NULL; }
    kiln_host_wave_free((KilnHostWave *)wav->st);
    memset(wav, 0, sizeof *wav);
}

/* ── trackers ─────────────────────────────────────────────────────────
 * Channel counts matter: kiln_audio reserves a music range and asks the player
 * how many channels it wants. A host reporting 0 would make the partition
 * arithmetic trivially satisfiable and hide an over-subscription. */
int xm64player_open(xm64player_t *p, const char *fn)
{
    assertf(p != NULL, "xm64player_open: NULL");
    memset(p, 0, sizeof *p);
    const int h = dfs_open(fn);
    if (h <= 0) { debugf("xm64player_open: '%s' is missing\n", fn); return -1; }
    dfs_close((uint32_t)h);
    p->channels = 10;   /* the report's figure for a typical XM here */
    p->vol = 1.0f;
    return 0;
}
void xm64player_play(xm64player_t *p, int first_ch)
{
    assertf(p != NULL, "xm64player_play: NULL");
    p->playing = 1; p->first_ch = first_ch;
    for (int i = 0; i < p->channels; i++) {
        const int ch = first_ch + i;
        if (ch < g_nch) g_ch[ch].playing = 1;
    }
}
void xm64player_stop(xm64player_t *p)
{
    if (!p) return;
    for (int i = 0; i < p->channels; i++) {
        const int ch = p->first_ch + i;
        if (ch >= 0 && ch < g_nch) g_ch[ch].playing = 0;
    }
    p->playing = 0;
}
void xm64player_close(xm64player_t *p) { if (p) memset(p, 0, sizeof *p); }
void xm64player_set_loop(xm64player_t *p, bool loop) { if (p) p->loop = loop; }
void xm64player_set_vol(xm64player_t *p, float volume) { if (p) p->vol = volume; }
int  xm64player_num_channels(xm64player_t *p) { return p ? p->channels : 0; }
/* No notes are played, so there is no cursor to move: the host reports the
 * start of the tune, which is what a real player reports before its first
 * tick, and accepts a seek without pretending it went anywhere. */
void xm64player_tell(xm64player_t *p, int *patidx, int *row, float *secs)
{
    (void)p;
    if (patidx) *patidx = 0;
    if (row) *row = 0;
    if (secs) *secs = 0.0f;
}
void xm64player_seek(xm64player_t *p, int patidx, int row, int tick)
{
    (void)p; (void)patidx; (void)row; (void)tick;
}

void ym64player_open(ym64player_t *p, const char *fn, ym64player_songinfo_t *info)
{
    assertf(p != NULL, "ym64player_open: NULL");
    memset(p, 0, sizeof *p);
    const int h = dfs_open(fn);
    if (h > 0) { dfs_close((uint32_t)h); p->channels = 3; }
    else debugf("ym64player_open: '%s' is missing\n", fn);
    if (info) { info->name = fn; info->channels = p->channels; }
}
void ym64player_play(ym64player_t *p, int first_ch)
{
    if (!p) return;
    p->playing = 1; p->first_ch = first_ch;
    for (int i = 0; i < p->channels; i++)
        if (first_ch + i < g_nch) g_ch[first_ch + i].playing = 1;
}
void ym64player_stop(ym64player_t *p)
{
    if (!p) return;
    for (int i = 0; i < p->channels; i++)
        if (p->first_ch + i >= 0 && p->first_ch + i < g_nch)
            g_ch[p->first_ch + i].playing = 0;
    p->playing = 0;
}
void ym64player_close(ym64player_t *p) { if (p) memset(p, 0, sizeof *p); }
int  ym64player_num_channels(ym64player_t *p) { return p ? p->channels : 0; }

/* rspq high-priority queue: kiln_audio brackets its mixer_poll with these so
 * audio jumps the display list. No queue here. */
void rspq_highpri_begin(void) { }
void rspq_highpri_end(void)   { }
void rspq_highpri_sync(void)  { }
