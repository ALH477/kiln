/* SPDX-License-Identifier: MIT
 *
 * host_audio.c — the mixer's bookkeeping, on the host. No samples.
 *
 * See plat/host/include/libdragon.h for the reasoning. Short version: nearly
 * all of kiln_audio is channel arithmetic — a 32-channel budget partitioned
 * into SFX and music ranges, priority-based voice stealing, room crossfades —
 * and that arithmetic is either right or produces a silence nobody can
 * explain. It needs no PCM to be checkable, and pretending to produce PCM
 * would be the politely-degrading failure this project keeps meeting.
 *
 * So the channel state is exact and the output is silence, said out loud once.
 */
#include <libdragon.h>
#include <kiln_host.h>
#include "host_internal.h"

#include <stdio.h>
#include <string.h>

#define MAX_CH 32

static struct { int playing; float lvol, rvol, freq; const waveform_t *wave; }
    g_ch[MAX_CH];
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
}
void mixer_close(void) { g_nch = 0; }
void mixer_set_vol(float vol) { g_master = vol; }

static void check_ch(int ch, const char *who)
{
    assertf(ch >= 0 && ch < g_nch,
            "%s: channel %d is outside the %d the mixer was initialised with",
            who, ch, g_nch);
}

void mixer_ch_play(int ch, waveform_t *wave)
{
    check_ch(ch, "mixer_ch_play");
    g_ch[ch].playing = 1;
    g_ch[ch].wave = wave;
    g_c.ch_plays++;
}
void mixer_ch_set_vol(int ch, float lvol, float rvol)
{
    check_ch(ch, "mixer_ch_set_vol");
    g_ch[ch].lvol = lvol; g_ch[ch].rvol = rvol;
}
void mixer_ch_set_vol_pan(int ch, float vol, float pan)
{
    /* libdragon's own mapping: pan 0 is hard left, 1 hard right. kiln_sound
     * computes pan from the listener basis, so getting this backwards would
     * put every positional sound on the wrong side. */
    check_ch(ch, "mixer_ch_set_vol_pan");
    g_ch[ch].lvol = vol * (1.0f - pan);
    g_ch[ch].rvol = vol * pan;
}
void mixer_ch_set_freq(int ch, float frequency)
{
    check_ch(ch, "mixer_ch_set_freq");
    g_ch[ch].freq = frequency;
}
void mixer_ch_stop(int ch)
{
    check_ch(ch, "mixer_ch_stop");
    g_ch[ch].playing = 0;
    g_ch[ch].wave = NULL;
    g_c.ch_stops++;
}
bool mixer_ch_playing(int ch)
{
    if (ch < 0 || ch >= g_nch) return false;
    return g_ch[ch].playing != 0;
}

void mixer_poll(int16_t *out, int nsamples)
{
    g_c.polls++;
    g_c.samples_requested += (uint32_t)(nsamples > 0 ? nsamples : 0);
    if (!g_said_silent) {
        g_said_silent = 1;
        debugf("host audio: mixer_poll produces SILENCE. Channel state is "
               "tracked exactly (see kiln_host_audio_counters) but no VADPCM "
               "decoding or output device is implemented.\n");
    }
    if (out && nsamples > 0) memset(out, 0, (size_t)nsamples * 2 * sizeof(int16_t));
}
void mixer_try_play(void) { }

void wav64_open(wav64_t *wav, const char *fn)
{
    assertf(wav != NULL, "wav64_open: NULL wav64_t");
    memset(wav, 0, sizeof *wav);
    g_c.wav_opens++;
    /* Through the host VFS, so a missing sound is a missing file rather than a
     * silence. This is the failure PetaByte Madness actually has — twelve
     * twelve sfx paths that flake.nix never builds — and it is exactly the
     * class the DFS gate was added for. */
    const int h = dfs_open(fn);
    if (h <= 0) {
        g_c.wav_missing++;
        debugf("wav64_open: '%s' is missing\n", fn);
        return;
    }
    const int sz = dfs_size((uint32_t)h);
    dfs_close((uint32_t)h);
    /* Enough for the mixer bookkeeping to be meaningful; the header is not
     * decoded because nothing here consumes samples. */
    wav->wave.channels = 1;
    wav->wave.bits = 16;
    wav->wave.frequency = g_freq ? g_freq : 32000;
    wav->wave.len = sz;
}

void wav64_play(wav64_t *wav, int ch)
{
    assertf(wav != NULL, "wav64_play: NULL");
    mixer_ch_play(ch, &wav->wave);
}
void wav64_close(wav64_t *wav) { if (wav) memset(wav, 0, sizeof *wav); }

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
