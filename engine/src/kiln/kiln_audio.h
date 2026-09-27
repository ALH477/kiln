/* SPDX-License-Identifier: MIT
 *
 * kiln_audio.h — the audio layer of Figulina.
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
 * follow-up — the API has fig_sfx_play_pan for manual positioning.
 */
#ifndef FIG_AUDIO_H
#define FIG_AUDIO_H

#include <libdragon.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum SFX handles (wav64 files loaded at boot). */
#define FIG_AUDIO_MAX_SFX 64

/** Maximum simultaneous music tracks (XM64/YM64 players). */
#define FIG_AUDIO_MAX_MUSIC 4

/** Audio init configuration. */
typedef struct {
    int sample_rate;     /**< AI sample rate (32000 recommended) */
    float latency;       /**< Audio latency in seconds (0.16 = 160 ms default) */
    int sfx_channels;    /**< Mixer channels for SFX (e.g. 16) */
    int music_channels;  /**< Mixer channels reserved for music (e.g. 10) */
} FigAudioConfig;

/** Default configuration: 32000 Hz, 160 ms latency, 16 SFX + 10 music channels. */
#define FIG_AUDIO_DEFAULT ((FigAudioConfig){ \
    .sample_rate = 32000, \
    .latency = 0.16f, \
    .sfx_channels = 16, \
    .music_channels = 10, \
})

/** Initialise audio and the mixer. Call after fig_engine_init.
 *  Total mixer channels = sfx_channels + music_channels (max 32).
 *  Asserts if the total exceeds MIXER_MAX_CHANNELS. */
void fig_audio_init(FigAudioConfig cfg);

/** Pump the audio mixer. Call once per frame, after fig_frame_end.
 *  Drains all available AI buffers via mixer_poll. */
void fig_audio_update(void);

/** Close audio and the mixer. */
void fig_audio_close(void);

/* ── SFX ────────────────────────────────────────────────────────────── */

/** Load a wav64 from DFS (e.g. "rom:/sfx/blip.wav64").
 *  Returns a non-negative handle, or -1 on failure.
 *  The wav64 is kept resident for the lifetime of the audio system. */
int fig_sfx_load(const char *dfs_path);

/** Play a loaded SFX. If `channel` < 0, auto-allocates from the SFX
 *  channel range, stealing the lowest-priority playing channel if all
 *  are busy. `priority` is used only for voice stealing (0 = never steal).
 *  Returns the channel used, or -1 if no channel was available.
 *
 *  A STEREO wav64 occupies two channels, the returned one and the next, as
 *  libdragon's mixer requires; the allocator finds (or steals) a pair, and
 *  every fig_sfx_* call made through the second half is routed to the first.
 *  Bake mono (`mono = true` in mkSound / mkBakedInstrument) when the two sides
 *  are the same signal — it halves the channels and the ROM bytes. */
int fig_sfx_play(int sfx_handle, int channel, int priority);

/** Play a loaded SFX with stereo volume and pan.
 *  `pan` is 0.0 (full left) to 1.0 (full right), 0.5 = centre.
 *  `vol` is 0.0 to 1.0. */
int fig_sfx_play_ex(int sfx_handle, int channel, int priority,
                    float vol, float pan);

/** Is the channel still playing? */
int fig_sfx_playing(int channel);

/** Stop a channel. */
void fig_sfx_stop(int channel);

/** Set a channel's volume and pan. */
void fig_sfx_set_vol_pan(int channel, float vol, float pan);

/** Set a channel's playback frequency in Hz (samples per second of source).
 *  This is ABSOLUTE: 1.0 means one sample a second, not "unchanged". */
void fig_sfx_set_freq(int channel, float freq);

/** Pitch a playing SFX by a ratio of the rate its wav64 was encoded at: 1.0 is
 *  unchanged, 2.0 an octave up. Call after fig_sfx_play*, which resets pitch.
 *
 *  libdragon's mixer ASSERTS when a channel's frequency exceeds its limit, and
 *  the default limit is the output rate — so an octave up on a 32 kHz asset at
 *  32 kHz output needs `mixer_ch_set_limits(ch, 16, 64000, 0)` first. Raising
 *  it grows that channel's sample buffer, which is why this layer does not do
 *  it for every channel on your behalf. */
void fig_sfx_set_pitch(int channel, float ratio);

/* ── Music (XM64/YM64) ──────────────────────────────────────────────── */

/** Load a tracker file from DFS (e.g. "rom:/music/theme.xm64").
 *  Returns a non-negative handle, or -1 on failure. */
int fig_music_load(const char *dfs_path);

/** Play a loaded music track. Uses the music channel range starting at
 *  sfx_channels. Loops by default. */
void fig_music_play(int music_handle);

/** Stop a music track. */
void fig_music_stop(int music_handle);

/** Set music volume (0.0 to 1.0). */
void fig_music_set_volume(int music_handle, float vol);

/** Set whether a music track loops. */
void fig_music_set_loop(int music_handle, int loop);

/** Is a music track playing?
 *
 *  For XM64 this is the PLAYER's state, not a mixer channel's. It used to ask
 *  whether the track's first mixer channel was playing, and libxm stops a
 *  channel on every tick that channel has no sample (xm64.c's sync loop), so a
 *  tune whose first column rests read as stopped for the length of the rest. */
int fig_music_playing(int music_handle);

/** Number of channels a music track uses. */
int fig_music_num_channels(int music_handle);

/** First mixer channel a playing track occupies, or -1 when it is not playing.
 *  For a visualiser reading mixer_ch_playing / mixer_ch_get_pos per channel. */
int fig_music_first_channel(int music_handle);

/** Where an XM64 track is: pattern index, row, and seconds played. Any output
 *  may be NULL. A YM64 track reports -1 for pattern and row. */
void fig_music_tell(int music_handle, int *pattern, int *row, float *secs);

/** Move an XM64 track's cursor (libdragon's xm64player_seek: effects active at
 *  that point are NOT reconstructed). fig_music_play resumes from the cursor,
 *  so stop + seek(0, 0) + play is a restart and stop + play is a resume. */
void fig_music_seek(int music_handle, int pattern, int row);

/* ── Output tap ─────────────────────────────────────────────────────── */

/** Called by fig_audio_update with every buffer it mixes, after mixer_poll
 *  returns (mixer_poll is synchronous) and before the buffer is handed to the
 *  AI: `samples` is `frames` interleaved stereo pairs. READ-ONLY by contract —
 *  it is the buffer the speaker gets. For meters and oscilloscopes that show
 *  what was actually mixed rather than a model of it. */
typedef void (*FigAudioTap)(const int16_t *samples, int frames, void *ctx);

/** Install (or with NULL, remove) the output tap. One at a time. */
void fig_audio_set_tap(FigAudioTap tap, void *ctx);

/* ── Output insert ──────────────────────────────────────────────────── */

/** Called by fig_audio_update with every buffer it mixes, after mixer_poll
 *  and BEFORE the tap: `samples` is `frames` interleaved stereo pairs, and
 *  unlike the tap this one MAY rewrite them in place.
 *
 *  ── What this is, and what it is not ──────────────────────────────────
 *  It is an insert effect across the WHOLE MIX. libdragon's mixer has no
 *  per-channel send and no insert point — it sums its channels into the output
 *  buffer and that buffer goes to the AI — so this is the only place in the
 *  pipeline an effect can sit at all. A caller that wants an effect on one
 *  piece of music installs it while that music is the only thing sounding and
 *  removes it after.
 *
 *  It runs before the tap on purpose, so a meter still shows what the speaker
 *  gets rather than what the mixer produced.
 *
 *  ── The cost is not optional ──────────────────────────────────────────
 *  This is per sample, on the VR4300, inside the loop that feeds the AI — the
 *  one part of the frame that preempts everything else (see the comment above
 *  the mixer_poll loop). An insert that overruns does not drop a frame of
 *  picture, it underruns the audio. Budget it: nix/faust.nix gates a Faust
 *  effect at a declared cycles-per-sample figure for exactly this reason.
 */
typedef void (*FigAudioInsert)(int16_t *samples, int frames, void *ctx);

/** Install (or with NULL, remove) the output insert. One at a time. */
void fig_audio_set_insert(FigAudioInsert fn, void *ctx);

/* ── Room-based audio routing ───────────────────────────────────────── */

/* The room system type is defined in kiln_room.h. We use void* here to
 * avoid a header dependency cycle — the caller passes the FigRoomSystem*
 * from kiln_room.h, and the implementation casts it. */

/** Associate a music track with a room ID. When the active room changes
 *  (via fig_audio_update_rooms), the engine crossfades to the new track.
 *  If the same track is assigned to both rooms, no transition occurs. */
void fig_audio_set_room_music(uint8_t room_id, int music_handle);

/** Check the room system's active room and transition music if needed.
 *  Call once per frame, after fig_room_system_update and before
 *  fig_audio_update. Performs a linear gain ramp crossfade (~0.5s).
 *  `room_sys` is a FigRoomSystem* (from kiln_room.h), passed as void*. */
void fig_audio_update_rooms(void *room_sys);

#ifdef __cplusplus
}
#endif

#endif /* FIG_AUDIO_H */