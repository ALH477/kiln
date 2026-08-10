/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_audio.c — audio layer implementation. See m64_audio.h for the model.
 *
 * A thin shell over libdragon's mixer, wav64, and XM64/YM64. The mixer
 * does the expensive work on the RSP; this layer manages channel allocation,
 * priority-based voice stealing, and the per-frame pump.
 */

#include "m64_audio.h"
#include "m64_room.h"

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
    int loop;         /* honoured at play time; 1 (loop) by default */
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
    /* wav64_open asserts on a missing file (asset.c's must_open), so a ROM
     * asking for a sound that has not been authored yet would die at boot
     * rather than run silent. Probe first and return the -1 this function
     * already documents. */
    if (!m64_dfs_exists(dfs_path)) {
        debugf("m64_sfx_load: no %s\n", dfs_path);
        return -1;
    }
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
    /* Same assert-on-missing as m64_sfx_load; same fix. */
    if (!m64_dfs_exists(dfs_path)) {
        debugf("m64_music_load: no %s\n", dfs_path);
        return -1;
    }

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
    g_music[idx].loop = 1;  /* the documented default; m64_music_set_loop overrides */
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
        /* Apply the track's stored loop flag rather than forcing `true`.
         * Forcing it here made m64_music_set_loop a no-op whenever it was
         * called BEFORE m64_music_play — which is the natural order, and
         * which silently turned a one-shot track into an endless one. */
        xm64player_set_loop(&g_music[music_handle].xm,
                            g_music[music_handle].loop ? true : false);
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
    /* Recorded either way, so a call made BEFORE m64_music_play still takes
     * effect when the track starts. */
    g_music[music_handle].loop = loop ? 1 : 0;
    if (g_music[music_handle].is_xm == 1) {
        xm64player_set_loop(&g_music[music_handle].xm, loop ? true : false);
    }
    /* YM64 loops by default; no per-track loop toggle in the API. */
}

int m64_music_playing(int music_handle)
{
    if (music_handle < 0 || music_handle >= g_music_count) return 0;
    if (g_music[music_handle].is_xm < 0) return 0;

    /* ANY of the track's channels, not just the first.
     *
     * A tracker channel is only "playing" while a note is sounding on it,
     * so testing the first channel alone reports a stopped song every time
     * that one voice rests. On a string quartet whose lead has 18 notes in
     * 64 seconds, that is most of the piece — and a caller using this to
     * detect end-of-song (the obvious use) fires within a few bars. */
    int first = g_music[music_handle].first_ch;
    if (first < 0) return 0;
    for (int ch = first; ch < first + g_music[music_handle].num_ch; ch++) {
        if (mixer_ch_playing(ch)) return 1;
    }
    return 0;
}

int m64_music_num_channels(int music_handle)
{
    if (music_handle < 0 || music_handle >= g_music_count) return 0;
    return g_music[music_handle].num_ch;
}

/* ── Room-based audio routing ──────────────────────────────────────── */

#define M64_AUDIO_MAX_ROOMS 64
#define M64_AUDIO_XFADE_FRAMES 16000  /* ~0.5s at 32000 Hz */

static struct {
    int music_handle;   /* -1 = no music for this room */
} g_room_music[M64_AUDIO_MAX_ROOMS];

static int g_room_active = -1;
static int g_room_prev_music = -1;
static int g_room_xfade_pos = -1;  /* -1 = no crossfade in progress */

void m64_audio_set_room_music(uint8_t room_id, int music_handle)
{
    if (room_id >= M64_AUDIO_MAX_ROOMS) return;
    g_room_music[room_id].music_handle = music_handle;
}

void m64_audio_update_rooms(void *room_sys)
{
    M64RoomSystem *sys = (M64RoomSystem *)room_sys;
    if (!sys || !g_audio.initialised) return;

    M64Room *current = m64_room_current(sys);
    int new_room = current ? current->id : -1;

    if (new_room != g_room_active) {
        g_room_active = new_room;

        /* Start a crossfade if we have a previous and/or new track. */
        int new_music = (new_room >= 0) ? g_room_music[new_room].music_handle : -1;

        /* If the same track is playing, don't restart — just keep going. */
        if (new_music == g_room_prev_music && new_music >= 0) return;

        if (g_room_prev_music >= 0) {
            /* Fade out the old track, then start the new one. */
            g_room_xfade_pos = 0;
        } else if (new_music >= 0) {
            /* No previous track: just start the new one. */
            m64_music_play(new_music);
            m64_music_set_volume(new_music, 0.0f);
            g_room_xfade_pos = 0;
            g_room_prev_music = new_music;
        }
    }

    /* Drive the crossfade. */
    if (g_room_xfade_pos >= 0) {
        float frac = (float)g_room_xfade_pos / (float)M64_AUDIO_XFADE_FRAMES;

        /* First half: fade out old. Second half: fade in new. */
        if (g_room_xfade_pos < M64_AUDIO_XFADE_FRAMES / 2) {
            /* Fade out */
            if (g_room_prev_music >= 0) {
                float vol = 1.0f - 2.0f * frac;
                m64_music_set_volume(g_room_prev_music, vol);
            }
        } else {
            /* Switch over at the midpoint. */
            if (g_room_prev_music >= 0 && frac < 0.55f) {
                m64_music_stop(g_room_prev_music);
                int new_music = (g_room_active >= 0)
                    ? g_room_music[g_room_active].music_handle : -1;
                if (new_music >= 0) {
                    m64_music_play(new_music);
                    m64_music_set_volume(new_music, 0.0f);
                }
                g_room_prev_music = new_music;
            }

            /* Fade in */
            if (g_room_prev_music >= 0) {
                float vol = 2.0f * (frac - 0.5f);
                m64_music_set_volume(g_room_prev_music, vol);
            }
        }

        g_room_xfade_pos += audio_get_buffer_length();
        if (g_room_xfade_pos >= M64_AUDIO_XFADE_FRAMES) {
            g_room_xfade_pos = -1;
            /* Ensure final volume is exactly 1.0 */
            if (g_room_prev_music >= 0)
                m64_music_set_volume(g_room_prev_music, 1.0f);
        }
    }
}