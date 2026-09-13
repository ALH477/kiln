// SPDX-License-Identifier: MIT
//
// cine_shots.h — the cinematic's camera: seven shots, cut between.
//
// Each shot is a short kiln_camkey table flown ONE-SHOT (loop = 0), with key
// times local to the shot. kiln_camkey_sample clamps a one-shot's end tangents
// to zero, so every move eases in and out, and a shot boundary is a clean cut.
// One continuous looping spline through all seven beats was tried first. It
// travelled between beats instead of cutting, and so it swept through
// whatever stood in between. The n64-modeling skill's "a cut should cut" is
// the rule this follows.
//
// Every key is authored against where cine_script.h puts the subject at that
// key's time (the times are in the comments). nix/checks/cinematic-cam.nix
// runs kiln_camlint over every shot. It also flies the whole loop at 30 Hz
// against the cast's measured boxes, so a key that puts the eye inside the
// ship, a wall or an actor fails the flake check rather than a capture.
//
// Free of libdragon: the check compiles it natively.

#ifndef CINE_SHOTS_H
#define CINE_SHOTS_H

#include <kiln/kiln_camkey.h>

#include "cine_script.h"

#define CINE_NEAR_Z 4.0f
#define CINE_FAR_Z  360.0f

typedef struct {
    float start, dur;
    const KilnCamKey *keys;
    int n;
    const char *title;      /* caption card for the shot's first seconds */
} CineShot;

/* 0-9 establishing: high over the front-right corner, craning down onto the
 * captain as he walks. goblin t=0 (45,21) t=4.5 (26,41) t=9 (-5,50) */
static const KilnCamKey CINE_SHOT_OPEN[] = {
    { 0.0f, {{  84.0f, 46.0f,  84.0f }}, {{  14.0f,  8.0f,  14.0f }} },
    { 4.5f, {{  68.0f, 28.0f,  80.0f }}, {{  22.0f,  9.0f,  36.0f }} },
    { 9.0f, {{  52.0f, 18.0f,  82.0f }}, {{   2.0f,  9.0f,  48.0f }} },
};
/* 9-15 low along the front wall: the captain shoulders past stack A (bump at
 * 9.6) and walks on. goblin t=9.6 (-9,49) t=15 (-38,32) */
static const KilnCamKey CINE_SHOT_CRATES[] = {
    { 0.0f, {{  24.0f, 11.0f,  84.0f }}, {{ -10.0f,  8.0f,  52.0f }} },
    { 6.0f, {{   8.0f, 16.0f,  86.0f }}, {{ -30.0f,  9.0f,  38.0f }} },
};
/* 15-22 across the pad: the droids working their stations round the ship. */
static const KilnCamKey CINE_SHOT_DROIDS[] = {
    { 0.0f, {{  18.0f, 14.0f,  50.0f }}, {{   0.0f,  9.0f,   0.0f }} },
    { 7.0f, {{ -40.0f, 20.0f,  40.0f }}, {{   0.0f,  8.0f,  -4.0f }} },
};
/* 22-30 over the goblin's shoulder at the bay door: it opens at 22.5 and the
 * aliens come through from 23. goblin stops at (-28,-42) by 28. */
static const KilnCamKey CINE_SHOT_DOOR[] = {
    { 0.0f, {{ -12.0f, 24.0f, -30.0f }}, {{ -64.0f, 12.0f, -98.0f }} },
    { 8.0f, {{  -6.0f, 18.0f, -48.0f }}, {{ -56.0f, 10.0f, -84.0f }} },
};
/* 30-38 the stand-off, reticle on the lead alien 30-36. lead (-46,-66),
 * late (-76,-60), goblin (-28,-42). */
static const KilnCamKey CINE_SHOT_CONTACT[] = {
    { 0.0f, {{   2.0f, 13.0f, -36.0f }}, {{ -44.0f, 11.0f, -64.0f }} },
    { 8.0f, {{  -2.0f, 21.0f, -20.0f }}, {{ -50.0f, 10.0f, -60.0f }} },
};
/* 38-47 the whole hangar from the front-left: the aliens turn back at 44. */
static const KilnCamKey CINE_SHOT_WIDE[] = {
    { 0.0f, {{ -62.0f, 44.0f,  70.0f }}, {{ -30.0f,  6.0f, -30.0f }} },
    { 9.0f, {{ -22.0f, 46.0f,  76.0f }}, {{ -10.0f,  6.0f, -30.0f }} },
};
/* 47-60 the captain resumes along the back of the pad and round the corner.
 * goblin t=47 (9,-49) t=53 (40,-30) t=60 (45,21) */
static const KilnCamKey CINE_SHOT_BACK[] = {
    {  0.0f, {{  40.0f, 12.0f, -84.0f }}, {{  10.0f,  8.0f, -48.0f }} },
    {  6.5f, {{  78.0f, 14.0f, -60.0f }}, {{  38.0f,  8.0f, -28.0f }} },
    { 13.0f, {{  86.0f, 22.0f,  30.0f }}, {{  44.0f,  8.0f,  10.0f }} },
};

#define CINE_SHOT(s, d, k, title) { s, d, k, (int)(sizeof(k) / sizeof(k[0])), title }
static const CineShot CINE_SHOTS[] = {
    CINE_SHOT( 0.0f,  9.0f, CINE_SHOT_OPEN,    "HANGAR 7"),
    CINE_SHOT( 9.0f,  6.0f, CINE_SHOT_CRATES,  "THE CAPTAIN"),
    CINE_SHOT(15.0f,  7.0f, CINE_SHOT_DROIDS,  "SERVICE DROIDS"),
    CINE_SHOT(22.0f,  8.0f, CINE_SHOT_DOOR,    "BAY DOOR"),
    CINE_SHOT(30.0f,  8.0f, CINE_SHOT_CONTACT, "CONTACT"),
    CINE_SHOT(38.0f,  9.0f, CINE_SHOT_WIDE,    "TRUCE"),
    CINE_SHOT(47.0f, 13.0f, CINE_SHOT_BACK,    "STAND DOWN"),
};
#undef CINE_SHOT
#define CINE_SHOT_COUNT ((int)(sizeof(CINE_SHOTS) / sizeof(CINE_SHOTS[0])))

/* The shot live at time t (0..CINE_LOOP_T). */
static inline int cine_shot_at(const CineShot *shots, int n, float t)
{
    int i = 0;
    while (i < n - 1 && t >= shots[i + 1].start) i++;
    return i;
}

/* Camera shake: a short decaying jolt on each impact — the crates going over,
 * the door slamming open and shut. A pure function of t, and applied HERE
 * rather than in the ROM, so nix/checks/cinematic-cam.nix flies the shaken eye
 * and its clearance test covers the shake too. */
typedef struct { float at, amp; } CineJolt;
static const CineJolt CINE_JOLTS[] = {
    {  9.6f, 0.9f },   /* stack A */
    { 22.5f, 0.6f },   /* door opens */
    { 29.5f, 0.8f },   /* stack B */
    { 55.0f, 0.5f },   /* door closes */
};
#define CINE_JOLT_LEN 0.6f

static inline fm_vec3_t cine_shake(float t)
{
    fm_vec3_t o = {{ 0, 0, 0 }};
    for (int i = 0; i < (int)(sizeof(CINE_JOLTS) / sizeof(CINE_JOLTS[0])); i++) {
        const float u = (t - CINE_JOLTS[i].at) / CINE_JOLT_LEN;
        if (u < 0.0f || u >= 1.0f) continue;
        const float k = CINE_JOLTS[i].amp * (1.0f - u) * (1.0f - u);
        o.v[0] += k * fm_sinf(t * 57.0f);
        o.v[1] += k * 0.6f * fm_sinf(t * 43.0f + 1.3f);
        o.v[2] += k * fm_cosf(t * 51.0f);
    }
    return o;
}

/* Eye and look at time t, flown through the live shot, shaken. */
static inline void cine_camera(const CineShot *shots, int n, float t,
                               fm_vec3_t *eye, fm_vec3_t *look)
{
    const CineShot *s = &shots[cine_shot_at(shots, n, t)];
    kiln_camkey_sample(s->keys, s->n, 0, t - s->start, eye, look);
    const fm_vec3_t k = cine_shake(t);
    for (int i = 0; i < 3; i++) {
        eye->v[i] += k.v[i];
        look->v[i] += k.v[i] * 0.4f;
    }
}

#endif // CINE_SHOTS_H
