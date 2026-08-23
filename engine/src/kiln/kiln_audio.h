/* SPDX-License-Identifier: MIT
 *
 * kiln_audio.h — the audio layer of the Kiln engine.
 *
 * A thin wrapper over libdragon's RSP-accelerated mixer, wav64 sample
 * playback, and XM64/YM64 tracker music. Exists so ROMs don't each
 * duplicate the same audio_init/mixer_init/mixer_poll boilerplate, and
 * so the engine can enforce consistent sample rates (Phase 4) and provide
 * room-based music routing (Phase 5) without each game reinventing it.
 *
 * ── Why a thin wrapper and not a full audio engine ────────────────────
 * libdragon's mixer already does the expensive work on the RSP: VADPCM
 * decoding, channel mixing, volume/pan. Re-implementing that on the
 * VR4300 would be slower and worse. This layer adds the things the raw
 * API doesn't give you: a fixed SFX table with priority-based voice
 * stealing, music handle management, and a per-frame pump that fits the
 * engine's frame bracket pattern.
 *
 * ── Channel layout ────────────────────────────────────────────────────
 * The mixer supports up to 32 channels (MIXER_MAX_CHANNELS). This layer
 * partitions them:
 *
 *   [0 .. sfx_channels)       SFX — one-shot wav64, auto-allocated
 *   [sfx_channels .. sfx_channels + music_channels)  Music — XM64/YM64
 *
 * The split is configured at init time. SFX auto-allocation walks the
 * SFX range for a free channel, or steals the lowest-priority one.
 *
 * ── No 3D positional audio ────────────────────────────────────────────
 * The N64 has 2 audio channels (stereo). HRTF is not feasible on the
 * VR4300. Simple distance-based volume/pan is possible but left as a
 * follow-up — the API has kiln_sfx_play_pan for manual positioning.
 */
#ifndef KILN_AUDIO_H
#define KILN_AUDIO_H

#include <libdragon.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum SFX handles (wav64 files loaded at boot). */
#define KILN_AUDIO_MAX_SFX 64

/** Maximum simultaneous music tracks (XM64/YM64 players). */
#define KILN_AUDIO_MAX_MUSIC 4

/** Audio init configuration. */
typedef struct {
    int sample_rate;     /**< AI sample rate (32000 recommended) */
    float latency;       /**< Audio latency in seconds (0.16 = 160 ms default) */
    int sfx_channels;    /**< Mixer channels for SFX (e.g. 16) */
    int music_channels;  /**< Mixer channels reserved for music (e.g. 10) */
} KilnAudioConfig;

/** Default configuration: 32000 Hz, 160 ms latency, 16 SFX + 10 music channels. */
#define KILN_AUDIO_DEFAULT ((KilnAudioConfig){ \
    .sample_rate = 32000, \
    .latency = 0.16f, \
    .sfx_channels = 16, \
    .music_channels = 10, \
})

/** Initialise audio and the mixer. Call after kiln_engine_init.
 *  Total mixer channels = sfx_channels + music_channels (max 32).
 *  Asserts if the total exceeds MIXER_MAX_CHANNELS. */
void kiln_audio_init(KilnAudioConfig cfg);

/** Pump the audio mixer. Call once per frame, after kiln_frame_end.
 *  Drains all available AI buffers via mixer_poll. */
void kiln_audio_update(void);

/** Close audio and the mixer. */
void kiln_audio_close(void);

/* ── SFX ────────────────────────────────────────────────────────────── */

/** Load a wav64 from DFS (e.g. "rom:/sfx/blip.wav64").
 *  Returns a non-negative handle, or -1 on failure.
 *  The wav64 is kept resident for the lifetime of the audio system. */
int kiln_sfx_load(const char *dfs_path);

/** Play a loaded SFX. If `channel` < 0, auto-allocates from the SFX
 *  channel range, stealing the lowest-priority playing channel if all
 *  are busy. `priority` is used only for voice stealing (0 = never steal).
 *  Returns the channel used, or -1 if no channel was available. */
int kiln_sfx_play(int sfx_handle, int channel, int priority);

/** Play a loaded SFX with stereo volume and pan.
 *  `pan` is 0.0 (full left) to 1.0 (full right), 0.5 = centre.
 *  `vol` is 0.0 to 1.0. */
int kiln_sfx_play_ex(int sfx_handle, int channel, int priority,
                    float vol, float pan);

/** Is the channel still playing? */
int kiln_sfx_playing(int channel);

/** Stop a channel. */
void kiln_sfx_stop(int channel);

/** Set a channel's volume and pan. */
void kiln_sfx_set_vol_pan(int channel, float vol, float pan);

/** Set a channel's playback frequency (pitch shift). */
void kiln_sfx_set_freq(int channel, float freq);

/* ── Music (XM64/YM64) ──────────────────────────────────────────────── */

/** Load a tracker file from DFS (e.g. "rom:/music/theme.xm64").
 *  Returns a non-negative handle, or -1 on failure. */
int kiln_music_load(const char *dfs_path);

/** Play a loaded music track. Uses the music channel range starting at
 *  sfx_channels. Loops by default. */
void kiln_music_play(int music_handle);

/** Stop a music track. */
void kiln_music_stop(int music_handle);

/** Set music volume (0.0 to 1.0). */
void kiln_music_set_volume(int music_handle, float vol);

/** Set whether a music track loops. */
void kiln_music_set_loop(int music_handle, int loop);

/** Is a music track playing? */
int kiln_music_playing(int music_handle);

/** Number of channels a music track uses. */
int kiln_music_num_channels(int music_handle);

/* ── Room-based audio routing ───────────────────────────────────────── */

/* The room system type is defined in kiln_room.h. We use void* here to
 * avoid a header dependency cycle — the caller passes the KilnRoomSystem*
 * from kiln_room.h, and the implementation casts it. */

/** Associate a music track with a room ID. When the active room changes
 *  (via kiln_audio_update_rooms), the engine crossfades to the new track.
 *  If the same track is assigned to both rooms, no transition occurs. */
void kiln_audio_set_room_music(uint8_t room_id, int music_handle);

/** Check the room system's active room and transition music if needed.
 *  Call once per frame, after kiln_room_system_update and before
 *  kiln_audio_update. Performs a linear gain ramp crossfade (~0.5s).
 *  `room_sys` is a KilnRoomSystem* (from kiln_room.h), passed as void*. */
void kiln_audio_update_rooms(void *room_sys);

#ifdef __cplusplus
}
#endif

#endif /* KILN_AUDIO_H */