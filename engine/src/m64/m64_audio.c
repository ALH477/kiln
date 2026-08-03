/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_audio.c — audio layer implementation. See m64_audio.h for the model.
 *
 * A thin shell over libdragon's mixer, wav64, and XM64/YM64. The mixer
 * does the expensive work on the RSP; this layer manages channel allocation,
 * priority-based voice stealing, and the per-frame pump.
 */

#include "m64_audio.h"

#include <string.h>

/* ── Module state ───────────────────────────────────────────────────── */

static struct {
    int sample_rate;
    int sfx_channels;     /* [0 .. sfx_channels) */
    int music_channels;   /* [sfx_channels .. sfx_channels + music_channels) */
    int total_channels;
    int initialised;
} g_audio;

/* SFX table: wav64 files loaded at boot, kept resident. */
static wav64_t g_sfx[M64_AUDIO_MAX_SFX];
static int g_sfx_count;

/* Per-SFX-channel priority (for voice stealing). 0 = not playing or no priority. */
static int g_ch_priority[MIXER_MAX_CHANNELS];

/* Music table: XM64/YM64 players. */
static struct {
    xm64player_t xm;
    ym64player_t ym;
    int is_xm;        /* 1 = XM64, 0 = YM64, -1 = empty slot */
    int first_ch;     /* first mixer channel assigned to this track */
    int num_ch;       /* channels this track occupies */
} g_music[M64_AUDIO_MAX_MUSIC];
static int g_music_count;

/* ── Public API ─────────────────────────────────────────────────────── */

void m64_audio_init(M64AudioConfig cfg)
{
    int total = cfg.sfx_channels + cfg.music_channels;
    assertf(total <= MIXER_MAX_CHANNELS,
            "m64_audio: %d SFX + %d music = %d channels, exceeds MIXER_MAX_CHANNELS (%d)",
            cfg.sfx_channels, cfg.music_channels, total, MIXER_MAX_CHANNELS);
    assertf(total > 0, "m64_audio: need at least 1 channel");

    audio_init(cfg.sample_rate, cfg.latency);
    mixer_init(total);

    g_audio.sample_rate = cfg.sample_rate;
    g_audio.sfx_channels = cfg.sfx_channels;
    g_audio.music_channels = cfg.music_channels;
    g_audio.total_channels = total;
    g_audio.initialised = 1;
    g_sfx_count = 0;
    g_music_count = 0;

    memset(g_ch_priority, 0, sizeof(g_ch_priority));
    for (int i = 0; i < M64_AUDIO_MAX_MUSIC; i++)
        g_music[i].is_xm = -1;
}

void m64_audio_update(void)
{
    if (!g_audio.initialised) return;

    /* Drain all available AI buffers. mixer_poll renders directly into the
     * buffer the AI will DMA, so this must be called often enough to stay
     * ahead of playback — once per frame is sufficient at typical latencies. */
    while (audio_can_write()) {
        short *buf = audio_write_begin();
        mixer_poll(buf, audio_get_buffer_length());
        audio_write_end();
    }
}

void m64_audio_close(void)
{
    if (!g_audio.initialised) return;

    for (int i = 0; i < g_sfx_count; i++)
        wav64_close(&g_sfx[i]);

    for (int i = 0; i < g_music_count; i++) {
        if (g_music[i].is_xm == 1) {
            xm64player_stop(&g_music[i].xm);
            xm64player_close(&g_music[i].xm);
        } else if (g_music[i].is_xm == 0) {
            ym64player_stop(&g_music[i].ym);
            ym64player_close(&g_music[i].ym);
        }
    }

    mixer_close();
    audio_close();
    g_audio.initialised = 0;
}

/* ── SFX ────────────────────────────────────────────────────────────── */

int m64_sfx_load(const char *dfs_path)
{
    if (!dfs_path || g_sfx_count >= M64_AUDIO_MAX_SFX) return -1;
    wav64_open(&g_sfx[g_sfx_count], dfs_path);
    return g_sfx_count++;
}

int m64_sfx_play(int sfx_handle, int channel, int priority)
{
    return m64_sfx_play_ex(sfx_handle, channel, priority, 1.0f, 0.5f);
}

int m64_sfx_play_ex(int sfx_handle, int channel, int priority,
                    float vol, float pan)
{
    if (sfx_handle < 0 || sfx_handle >= g_sfx_count) return -1;
    if (!g_audio.initialised) return -1;

    /* Auto-allocate a channel if none specified. */
    if (channel < 0) {
        /* First, look for a free channel in the SFX range. */
        for (int ch = 0; ch < g_audio.sfx_channels; ch++) {
            if (!mixer_ch_playing(ch)) {
                channel = ch;
                break;
            }
        }
        /* All busy: steal the lowest-priority channel if our priority is higher. */
        if (channel < 0) {
            int victim = -1;
            int lowest_pri = priority;
            for (int ch = 0; ch < g_audio.sfx_channels; ch++) {
                if (g_ch_priority[ch] < lowest_pri) {
                    lowest_pri = g_ch_priority[ch];
                    victim = ch;
                }
            }
            if (victim >= 0) {
                mixer_ch_stop(victim);
                channel = victim;
            }
        }
        if (channel < 0) return -1; /* all channels busy at >= our priority */
    }

    /* Clamp channel to SFX range. */
    if (channel >= g_audio.sfx_channels) return -1;

    wav64_play(&g_sfx[sfx_handle], channel);
    g_ch_priority[channel] = priority;

    /* Volume and pan: libdragon wants separate L/R volumes. */
    float lvol = vol * (1.0f - pan);
    float rvol = vol * pan;
    mixer_ch_set_vol(channel, lvol, rvol);

    return channel;
}

int m64_sfx_playing(int channel)
{
    if (channel < 0 || channel >= g_audio.sfx_channels) return 0;
    return mixer_ch_playing(channel);
}

void m64_sfx_stop(int channel)
{
    if (channel < 0 || channel >= g_audio.sfx_channels) return;
    mixer_ch_stop(channel);
    g_ch_priority[channel] = 0;
}

void m64_sfx_set_vol_pan(int channel, float vol, float pan)
{
    if (channel < 0 || channel >= g_audio.sfx_channels) return;
    float lvol = vol * (1.0f - pan);
    float rvol = vol * pan;
    mixer_ch_set_vol(channel, lvol, rvol);
}

void m64_sfx_set_freq(int channel, float freq)
{
    if (channel < 0 || channel >= g_audio.sfx_channels) return;
    mixer_ch_set_freq(channel, freq);
}

/* ── Music ──────────────────────────────────────────────────────────── */

int m64_music_load(const char *dfs_path)
{
    if (!dfs_path || g_music_count >= M64_AUDIO_MAX_MUSIC) return -1;

    int idx = g_music_count;
    /* Detect XM vs YM by extension. */
    const char *dot = strrchr(dfs_path, '.');
    int is_xm = 1;
    if (dot) {
        if (dot[1] == 'y' || dot[1] == 'Y')
            is_xm = 0;
    }

    if (is_xm) {
        xm64player_open(&g_music[idx].xm, dfs_path);
        g_music[idx].is_xm = 1;
    } else {
        ym64player_open(&g_music[idx].ym, dfs_path, NULL);
        g_music[idx].is_xm = 0;
    }

    g_music[idx].first_ch = -1;
    g_music[idx].num_ch = 0;
    return g_music_count++;
}

void m64_music_play(int music_handle)
{
    if (music_handle < 0 || music_handle >= g_music_count) return;
    if (g_music[music_handle].is_xm < 0) return;

    /* Assign mixer channels from the music range. Each track gets channels
     * starting at the first free slot in [sfx_channels .. total). */
    int first = g_audio.sfx_channels;
    for (int i = 0; i < g_music_count; i++) {
        if (g_music[i].is_xm < 0) continue;
        if (i == music_handle) continue;
        if (g_music[i].first_ch >= 0)
            first = g_music[i].first_ch + g_music[i].num_ch;
    }

    if (g_music[music_handle].is_xm == 1) {
        int n = xm64player_num_channels(&g_music[music_handle].xm);
        g_music[music_handle].first_ch = first;
        g_music[music_handle].num_ch = n;
        xm64player_set_loop(&g_music[music_handle].xm, true);
        xm64player_play(&g_music[music_handle].xm, first);
    } else {
        int n = ym64player_num_channels(&g_music[music_handle].ym);
        g_music[music_handle].first_ch = first;
        g_music[music_handle].num_ch = n;
        ym64player_play(&g_music[music_handle].ym, first);
    }
}

void m64_music_stop(int music_handle)
{
    if (music_handle < 0 || music_handle >= g_music_count) return;
    if (g_music[music_handle].is_xm < 0) return;

    if (g_music[music_handle].is_xm == 1) {
        xm64player_stop(&g_music[music_handle].xm);
    } else {
        ym64player_stop(&g_music[music_handle].ym);
    }
    g_music[music_handle].first_ch = -1;
}

void m64_music_set_volume(int music_handle, float vol)
{
    if (music_handle < 0 || music_handle >= g_music_count) return;
    if (g_music[music_handle].is_xm < 0) return;

    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;

    if (g_music[music_handle].is_xm == 1) {
        xm64player_set_vol(&g_music[music_handle].xm, vol);
    } else {
        /* YM64 doesn't have a per-player volume; set channel volumes. */
        int first = g_music[music_handle].first_ch;
        int n = g_music[music_handle].num_ch;
        for (int i = 0; i < n; i++)
            mixer_ch_set_vol(first + i, vol, vol);
    }
}

void m64_music_set_loop(int music_handle, int loop)
{
    if (music_handle < 0 || music_handle >= g_music_count) return;
    if (g_music[music_handle].is_xm == 1) {
        xm64player_set_loop(&g_music[music_handle].xm, loop ? true : false);
    }
    /* YM64 loops by default; no per-track loop toggle in the API. */
}

int m64_music_playing(int music_handle)
{
    if (music_handle < 0 || music_handle >= g_music_count) return 0;
    if (g_music[music_handle].is_xm < 0) return 0;

    /* Check if the first channel is still playing. */
    int first = g_music[music_handle].first_ch;
    if (first < 0) return 0;
    return mixer_ch_playing(first);
}

int m64_music_num_channels(int music_handle)
{
    if (music_handle < 0 || music_handle >= g_music_count) return 0;
    return g_music[music_handle].num_ch;
}