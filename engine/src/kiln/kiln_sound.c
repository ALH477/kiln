/* SPDX-License-Identifier: MIT
 *
 * kiln_sound.c — see kiln_sound.h for the model.
 */

#include "kiln_sound.h"

#include "kiln_audio.h"

#include <libdragon.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    KilnSoundShader def;
    int          sfx_handle; /** c kiln_sfx_load handle, -1 = invalid     */
    int          channel;    /** -1 = not currently playing              */
    fm_vec3_t    pos;
    float        pitch;
    int          active;
} ShaderChannel;

static struct {
    ShaderChannel ch[KILN_SOUND_CHANNELS];
    fm_vec3_t     listener_pos;
    fm_vec3_t     listener_facing;
    int           listener_valid;
} g_sound;

static float saturate(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

/* Helper: project `a` onto unit `axis`, then clamp and remap to [0,1].
 * Pan is 0 = full left, 1 = full right, 0.5 = centre. */
static float pan_from_direction(fm_vec3_t to_source, fm_vec3_t listener_right)
{
    float proj = fm_vec3_dot(&to_source, &listener_right);
    proj = saturate(proj * 0.5f + 0.5f);
    return proj;
}

void kiln_sound_init(const KilnSoundShader *shaders, int count)
{
    memset(&g_sound, 0, sizeof(g_sound));
    if (count < 0) count = 0;
    if (count > KILN_SOUND_CHANNELS) {
        debugf("kiln_sound: %d shaders requested but the table holds %d "
               "(KILN_SOUND_CHANNELS); the excess will never play\n", count, KILN_SOUND_CHANNELS);
        count = KILN_SOUND_CHANNELS;
    }

    for (int i = 0; i < count; i++) {
        g_sound.ch[i].def = shaders[i];
        g_sound.ch[i].sfx_handle = kiln_sfx_load(shaders[i].wav64_path);
        g_sound.ch[i].channel = -1;
    }
}

void kiln_sound_update_listener(fm_vec3_t pos, fm_vec3_t facing)
{
    g_sound.listener_pos = pos;
    g_sound.listener_facing = facing;
    g_sound.listener_valid = 1;
    fm_vec3_norm(&g_sound.listener_facing, &g_sound.listener_facing);
}

static void apply_positional(int idx)
{
    ShaderChannel *c = &g_sound.ch[idx];
    if (!g_sound.listener_valid || c->channel < 0) return;

    fm_vec3_t diff;
    fm_vec3_sub(&diff, &c->pos, &g_sound.listener_pos);
    float dist = fm_vec3_len(&diff);

    float vol = c->def.base_vol;
    if (c->def.falloff_radius > 0.0f) {
        float t = 1.0f - (dist / c->def.falloff_radius);
        vol *= saturate(t);
    }

    /* Listener right vector: rotate facing 90° around Y. */
    fm_vec3_t right = {{ -g_sound.listener_facing.v[2], 0.0f, g_sound.listener_facing.v[0] }};
    float pan = 0.5f;
    if (dist > 1e-3f) {
        fm_vec3_t dir = diff;
        fm_vec3_norm(&dir, &dir);
        pan = pan_from_direction(dir, right);
    }

    kiln_sfx_set_vol_pan(c->channel, vol, pan);
}

int kiln_sound_play(const char *name, fm_vec3_t world_pos, float pitch)
{
    if (!name) return -1;

    /* Single scan: find the shader slot by name. */
    int idx = -1;
    for (int i = 0; i < KILN_SOUND_CHANNELS; i++) {
        if (g_sound.ch[i].def.name && strcmp(g_sound.ch[i].def.name, name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        debugf("kiln_sound: unknown shader '%s'\n", name);
        return -1;
    }
    const KilnSoundShader *sh = &g_sound.ch[idx].def;
    ShaderChannel *c = &g_sound.ch[idx];
    if (c->sfx_handle < 0) return -1;

    int ch = kiln_sfx_play_ex(c->sfx_handle, -1, 1, sh->base_vol, 0.5f);
    if (ch < 0) return -1;
    c->channel = ch;
    c->pos = world_pos;
    c->pitch = pitch;
    c->active = 1;
    /* A RATIO. This passed `pitch` to kiln_sfx_set_freq, which takes Hz, so
     * any pitch other than exactly 1 played its sample at a couple of samples
     * per second — silence that never ended. Every caller so far passes 1. */
    if (pitch != 1.0f) kiln_sfx_set_pitch(ch, pitch);
    apply_positional(idx);
    return ch;
}

void kiln_sound_stop(int channel)
{
    if (channel < 0) return;
    kiln_sfx_stop(channel);
    for (int i = 0; i < KILN_SOUND_CHANNELS; i++) {
        if (g_sound.ch[i].channel == channel) {
            g_sound.ch[i].channel = -1;
            g_sound.ch[i].active = 0;
        }
    }
}

void kiln_sound_update(void)
{
    for (int i = 0; i < KILN_SOUND_CHANNELS; i++) {
        if (!g_sound.ch[i].active) continue;
        if (g_sound.ch[i].channel >= 0 && kiln_sfx_playing(g_sound.ch[i].channel)) {
            apply_positional(i);
        } else {
            g_sound.ch[i].channel = -1;
            g_sound.ch[i].active = 0;
        }
    }
}