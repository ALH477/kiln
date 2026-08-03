/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_sound.h — sound shaders. A clean-room analogue of id Tech 4's sound
 * shader system, reduced to what an N64 game can afford.
 *
 * ── Name → wav64 path + parameters ──────────────────────────────────────
 * Doom 3's sound shaders map a logical name (e.g. "player_step_default") to
 * one or more samples, volume, looping, and falloff. On N64 we keep a single
 * wav64 per shader, with base volume, falloff radius, and loop flag. Games
 * call `m64_sound_play("player_step_default", world_pos, pitch)`; the layer
 * looks up the shader, computes distance-based volume and facing-based pan,
 * and triggers m64_sfx_play_ex. Reusing the existing m64_sfx channel range
 * means no extra mixer bookkeeping.
 *
 * ── Positional audio: distance + pan only ───────────────────────────────
 * The N64 is stereo; HRTF is not feasible on the VR4300. Distance → volume
 * (linear falloff inside falloff_radius, silent outside). Listener-facing
 * → pan (world-right projected onto listener's right vector). This is the
 * same directional approximation Doom 3 used for many non-occluded sounds;
 * it is good enough for footsteps, projectiles, and ambient sources.
 *
 * ── One listener update per frame ─────────────────────────────────────────
 * `m64_sound_update_listener` is called once per frame with the camera/ear
 * position and facing direction. It updates the volume/pan of any shader
 * channel that is still playing. One-shot sounds do not need update; looping
 * positional sounds (torches, machines) do.
 */
#ifndef M64_SOUND_H
#define M64_SOUND_H

#include <t3d/t3dmath.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M64_SOUND_SHADER_MAX 64
#define M64_SOUND_CHANNELS   8

typedef struct {
    const char *name;       /**< logical name, e.g. "sfx/step_stone"          */
    const char *wav64_path; /**< rom:/ DFS path to the sample               */
    float base_vol;         /**< 0..1                                          */
    float falloff_radius;   /**< world units; 0 = no falloff (always full vol) */
    int   loop;             /**< non-zero if the sample should loop          */
} M64SoundShader;

/** Register all sound shaders at boot. `shaders` is a pointer to a static
 *  array; the table is copied into module state, so the array need not live
 *  forever. The wav64 files are loaded immediately and kept resident. */
void m64_sound_init(const M64SoundShader *shaders, int count);

/** Update the listener position and facing for positional calculations.
 *  Call once per frame, before m64_audio_update. */
void m64_sound_update_listener(fm_vec3_t pos, fm_vec3_t facing);

/** Play a sound shader by name at a world position with an optional pitch
 *  shift. Returns the channel used or -1. */
int m64_sound_play(const char *name, fm_vec3_t world_pos, float pitch);

/** Stop a looping sound started with m64_sound_play. */
void m64_sound_stop(int channel);

/** Per-frame update: recompute vol/pan for active shader channels. Call
 *  once per frame after updating the listener. */
void m64_sound_update(void);

#ifdef __cplusplus
}
#endif

#endif /* M64_SOUND_H */