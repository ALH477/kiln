/* SPDX-License-Identifier: MIT
 *
 * kiln_audio.c — audio layer implementation. See kiln_audio.h for the model.
 *
 * A thin shell over libdragon's mixer, wav64, and XM64/YM64. The mixer
 * does the expensive work on the RSP; this layer manages channel allocation,
 * priority-based voice stealing, and the per-frame pump.
 */

#include "kiln_audio.h"
#include "kiln_room.h"

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
static wav64_t g_sfx[KILN_AUDIO_MAX_SFX];
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
} g_music[KILN_AUDIO_MAX_MUSIC];
static int g_music_count;

/* ── Public API ─────────────────────────────────────────────────────── */

void kiln_audio_init(KilnAudioConfig cfg)
{
    int total = cfg.sfx_channels + cfg.music_channels;
    assertf(total <= MIXER_MAX_CHANNELS,
            "kiln_audio: %d SFX + %d music = %d channels, exceeds MIXER_MAX_CHANNELS (%d)",
            cfg.sfx_channels, cfg.music_channels, total, MIXER_MAX_CHANNELS);
    assertf(total > 0, "kiln_audio: need at least 1 channel");

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
    for (int i = 0; i < KILN_AUDIO_MAX_MUSIC; i++)
        g_music[i].is_xm = -1;
}

void kiln_audio_update(void)
{
    if (!g_audio.initialised) return;

    /* Drain all available AI buffers. mixer_poll renders directly into the
     * buffer the AI will DMA, so this must be called often enough to stay
     * ahead of playback.
     *
     * ── Why the mixing runs at HIGH PRIORITY ────────────────────────────
     * mixer_poll does its work on the RSP, and on this engine so does
     * Tiny3D. Queued normally, the mix waits behind however much geometry
     * the frame has already submitted — and "however much" is not a number
     * this layer can know or bound. A game that grows a sky dome, a sea and
     * denser terrain does not expect its audio to break, but that is what
     * happens: measured on PetaByte Madness' flyover, the mixer missed a
     * buffer roughly once per buffer cycle and the output sat at exactly
     * digital zero for milliseconds at a time, in every playback path at
     * once, because they all end here. The Kiln boot jingle stayed clean
     * through all of it, which was the clue — it plays over a splash screen
     * that queues almost nothing.
     *
     * rspq_highpri_begin makes the RSP switch to this work "almost
     * instantly (as soon as the current command is done), pausing the
     * normal queue" (rspq.h), and libdragon names audio as the intended
     * user of that facility. So the mix is no longer scheduled behind the
     * frame; it preempts it. Video has a whole frame of slack, audio has
     * none.
     *
     * Ordering still helps and callers should still pump early, but this is
     * what makes the guarantee independent of how heavy a frame gets. */
    while (audio_can_write()) {
        short *buf = audio_write_begin();
        rspq_highpri_begin();
        mixer_poll(buf, audio_get_buffer_length());
        rspq_highpri_end();
        audio_write_end();
    }
}

void kiln_audio_close(void)
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

int kiln_sfx_load(const char *dfs_path)
{
    if (!dfs_path || g_sfx_count >= KILN_AUDIO_MAX_SFX) return -1;
    wav64_open(&g_sfx[g_sfx_count], dfs_path);
    return g_sfx_count++;
}

int kiln_sfx_play(int sfx_handle, int channel, int priority)
{
    return kiln_sfx_play_ex(sfx_handle, channel, priority, 1.0f, 0.5f);
}

int kiln_sfx_play_ex(int sfx_handle, int channel, int priority,
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

int kiln_sfx_playing(int channel)
{
    if (channel < 0 || channel >= g_audio.sfx_channels) return 0;
    return mixer_ch_playing(channel);
}

void kiln_sfx_stop(int channel)
{
    if (channel < 0 || channel >= g_audio.sfx_channels) return;
    mixer_ch_stop(channel);
    g_ch_priority[channel] = 0;
}

void kiln_sfx_set_vol_pan(int channel, float vol, float pan)
{
    if (channel < 0 || channel >= g_audio.sfx_channels) return;
    float lvol = vol * (1.0f - pan);
    float rvol = vol * pan;
    mixer_ch_set_vol(channel, lvol, rvol);
}

void kiln_sfx_set_freq(int channel, float freq)
{
    if (channel < 0 || channel >= g_audio.sfx_channels) return;
    mixer_ch_set_freq(channel, freq);
}

/* ── Music ──────────────────────────────────────────────────────────── */

int kiln_music_load(const char *dfs_path)
{
    if (!dfs_path || g_music_count >= KILN_AUDIO_MAX_MUSIC) return -1;

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

void kiln_music_play(int music_handle)
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

void kiln_music_stop(int music_handle)
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

void kiln_music_set_volume(int music_handle, float vol)
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

void kiln_music_set_loop(int music_handle, int loop)
{
    if (music_handle < 0 || music_handle >= g_music_count) return;
    if (g_music[music_handle].is_xm == 1) {
        xm64player_set_loop(&g_music[music_handle].xm, loop ? true : false);
    }
    /* YM64 loops by default; no per-track loop toggle in the API. */
}

int kiln_music_playing(int music_handle)
{
    if (music_handle < 0 || music_handle >= g_music_count) return 0;
    if (g_music[music_handle].is_xm < 0) return 0;

    /* Check if the first channel is still playing. */
    int first = g_music[music_handle].first_ch;
    if (first < 0) return 0;
    return mixer_ch_playing(first);
}

int kiln_music_num_channels(int music_handle)
{
    if (music_handle < 0 || music_handle >= g_music_count) return 0;
    return g_music[music_handle].num_ch;
}

/* ── Room-based audio routing ──────────────────────────────────────── */

#define KILN_AUDIO_MAX_ROOMS 64
#define KILN_AUDIO_XFADE_FRAMES 16000  /* ~0.5s at 32000 Hz */

static struct {
    int music_handle;   /* -1 = no music for this room */
} g_room_music[KILN_AUDIO_MAX_ROOMS];

static int g_room_active = -1;
static int g_room_prev_music = -1;
static int g_room_xfade_pos = -1;  /* -1 = no crossfade in progress */

void kiln_audio_set_room_music(uint8_t room_id, int music_handle)
{
    if (room_id >= KILN_AUDIO_MAX_ROOMS) return;
    g_room_music[room_id].music_handle = music_handle;
}

void kiln_audio_update_rooms(void *room_sys)
{
    KilnRoomSystem *sys = (KilnRoomSystem *)room_sys;
    if (!sys || !g_audio.initialised) return;

    KilnRoom *current = kiln_room_current(sys);
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
            kiln_music_play(new_music);
            kiln_music_set_volume(new_music, 0.0f);
            g_room_xfade_pos = 0;
            g_room_prev_music = new_music;
        }
    }

    /* Drive the crossfade. */
    if (g_room_xfade_pos >= 0) {
        float frac = (float)g_room_xfade_pos / (float)KILN_AUDIO_XFADE_FRAMES;

        /* First half: fade out old. Second half: fade in new. */
        if (g_room_xfade_pos < KILN_AUDIO_XFADE_FRAMES / 2) {
            /* Fade out */
            if (g_room_prev_music >= 0) {
                float vol = 1.0f - 2.0f * frac;
                kiln_music_set_volume(g_room_prev_music, vol);
            }
        } else {
            /* Switch over at the midpoint. */
            if (g_room_prev_music >= 0 && frac < 0.55f) {
                kiln_music_stop(g_room_prev_music);
                int new_music = (g_room_active >= 0)
                    ? g_room_music[g_room_active].music_handle : -1;
                if (new_music >= 0) {
                    kiln_music_play(new_music);
                    kiln_music_set_volume(new_music, 0.0f);
                }
                g_room_prev_music = new_music;
            }

            /* Fade in */
            if (g_room_prev_music >= 0) {
                float vol = 2.0f * (frac - 0.5f);
                kiln_music_set_volume(g_room_prev_music, vol);
            }
        }

        g_room_xfade_pos += audio_get_buffer_length();
        if (g_room_xfade_pos >= KILN_AUDIO_XFADE_FRAMES) {
            g_room_xfade_pos = -1;
            /* Ensure final volume is exactly 1.0 */
            if (g_room_prev_music >= 0)
                kiln_music_set_volume(g_room_prev_music, 1.0f);
        }
    }
}