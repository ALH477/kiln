// SPDX-License-Identifier: MIT
//
// kiln_camlint.c — see kiln_camlint.h.
//
// NOTHING in this file may include libdragon or Tiny3D. It is compiled twice:
// once into a game's own ROM alongside its runtime, and once natively by
// nix/checks/kiln-logic.nix against nix/checks/stub/. The second build is
// what proves the detector fires in both directions, and it stops working
// the moment this file grows a dependency on the console.
//
// For the same reason the vector arithmetic below is written out against
// `.v[]` rather than reaching for fm_vec3_sub and friends: the surface this
// file needs from t3dmath.h is exactly one TYPE, and keeping it that way means
// the stub can never disagree with the real header about behaviour it does not
// implement.

#include "kiln_camlint.h"

#include <math.h>
#include <stddef.h>

// ── Small vector helpers, deliberately local ───────────────────────────
static float v_dist(fm_vec3_t a, fm_vec3_t b)
{
    const float dx = a.v[0] - b.v[0];
    const float dy = a.v[1] - b.v[1];
    const float dz = a.v[2] - b.v[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static int v_bad(fm_vec3_t a)
{
    for (int i = 0; i < 3; i++)
        if (isnan(a.v[i]) || isinf(a.v[i])) return 1;
    return 0;
}

/** Distance from `p` to the SEGMENT ab — not to the infinite line. Overshoot
 *  that runs past an endpoint is exactly the failure being measured, and a
 *  line-distance would score it as zero. */
static float dist_to_segment(fm_vec3_t p, fm_vec3_t a, fm_vec3_t b)
{
    float ab[3], ap[3];
    for (int i = 0; i < 3; i++) {
        ab[i] = b.v[i] - a.v[i];
        ap[i] = p.v[i] - a.v[i];
    }
    const float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = 0.0f;
    if (len2 > 0.0f) {
        t = (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
    }
    float d2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        const float d = ap[i] - ab[i] * t;
        d2 += d * d;
    }
    return sqrtf(d2);
}

// ── Names ──────────────────────────────────────────────────────────────
// Short and stable: they are drawn into a 320-pixel-wide report on console and
// printed by the host check, and both want the same word for the same bit.
const char *kiln_camlint_err_name(uint32_t b)
{
    switch (b) {
    case KILN_CAMLINT_ERR_NO_KEYS:      return "nokeys";
    case KILN_CAMLINT_ERR_TIME_ORDER:   return "order";
    case KILN_CAMLINT_ERR_KEY_PAST_END: return "pastend";
    case KILN_CAMLINT_ERR_DEGENERATE:   return "degen";
    case KILN_CAMLINT_ERR_FRUSTUM:      return "frustum";
    case KILN_CAMLINT_ERR_SUBJECT_CUT:  return "subjcut";
    case KILN_CAMLINT_ERR_NAN:          return "nan";
    default:                       return "?";
    }
}

const char *kiln_camlint_note_name(uint32_t b)
{
    switch (b) {
    case KILN_CAMLINT_NOTE_LATE_START: return "latestart";
    case KILN_CAMLINT_NOTE_DEAD_TAIL:  return "deadtail";
    case KILN_CAMLINT_NOTE_OVERSHOOT:  return "overshoot";
    case KILN_CAMLINT_NOTE_HITCH:      return "hitch";
    case KILN_CAMLINT_NOTE_OUTSIDE:    return "outside";
    case KILN_CAMLINT_NOTE_NEAR_AIM:   return "nearaim";
    default:                      return "?";
    }
}

// ── The validator ──────────────────────────────────────────────────────
uint32_t kiln_camlint(const KilnCamShot *shot, const KilnCamBounds *bounds,
                      KilnCamReport *out)
{
    KilnCamReport r;
    r.err = r.note = 0u;
    r.bad_key = -1;
    r.overshoot = 0.0f;
    r.overshoot_seg = -1;
    r.speed_max = 0.0f;
    r.speed_min = 0.0f;
    r.speed_ratio = 1.0f;
    r.subject_dist = 0.0f;
    r.tail = 0.0f;
    r.outside_key = -1;

    if (!shot || !shot->keys || shot->key_count <= 0) {
        r.err |= KILN_CAMLINT_ERR_NO_KEYS;
        if (out) *out = r;
        return r.err;
    }

    const KilnCamKey *k = shot->keys;
    const int n = shot->key_count;

    // ── Frustum, once ──────────────────────────────────────────────────
    // Checked before the per-key work because SUBJECT_CUT below is measured
    // against far_z, and reporting "the subject is past the far plane" for a
    // far plane that is itself nonsense would point at the wrong line.
    if (shot->near_z < 0.0f || shot->far_z <= shot->near_z)
        r.err |= KILN_CAMLINT_ERR_FRUSTUM;
    if (isnan(shot->near_z) || isnan(shot->far_z) ||
        isnan(shot->duration) || isinf(shot->duration))
        r.err |= KILN_CAMLINT_ERR_NAN;

    // ── Per key ────────────────────────────────────────────────────────
    for (int i = 0; i < n; i++) {
        const int first = (r.bad_key < 0);

        if (isnan(k[i].t) || isinf(k[i].t) || v_bad(k[i].eye) ||
            v_bad(k[i].look)) {
            r.err |= KILN_CAMLINT_ERR_NAN;
            if (first) r.bad_key = i;
            continue;   /* every measurement below would be NaN too */
        }

        // Strictly increasing. Equal times are a defect of the same kind as
        // decreasing ones rather than a "hard cut": the segment they bound has
        // zero span, so kiln_camkey_sample's `f` is 0 throughout it and the
        // second key of the pair is never reached. That is dead data wearing
        // the costume of a deliberate choice.
        if (i > 0 && k[i].t <= k[i - 1].t) {
            r.err |= KILN_CAMLINT_ERR_TIME_ORDER;
            if (first) r.bad_key = i;
        }

        // Unreachable, in a runtime that clamps its elapsed-time counter at
        // `duration`: a key beyond it is never the near end of a bracketing
        // pair and never contributes anything but a tangent.
        if (k[i].t > shot->duration + 1e-3f) {
            r.err |= KILN_CAMLINT_ERR_KEY_PAST_END;
            if (first) r.bad_key = i;
        }

        const float d = v_dist(k[i].eye, k[i].look);
        if (d > r.subject_dist) r.subject_dist = d;

        // The NaN halt CLAUDE.md's CAM section documents: kiln_camera
        // normalises look-eye, and a zero-length vector normalises to NaN,
        // which the VR4300 raises as "floating point invalid operation"
        // inside t3d_viewport_attach — several layers away from the table
        // that caused it.
        if (d <= 0.0f) {
            r.err |= KILN_CAMLINT_ERR_DEGENERATE;
            if (first) r.bad_key = i;
        }

        // The aim point is past the far plane, so whatever the shot is POINTED
        // AT is not drawn. This class of defect has cost multiple shots their
        // geometry in real use, and it is entirely static.
        if (!(r.err & KILN_CAMLINT_ERR_FRUSTUM) && d > shot->far_z) {
            r.err |= KILN_CAMLINT_ERR_SUBJECT_CUT;
            if (first) r.bad_key = i;
        }
        if (!(r.err & KILN_CAMLINT_ERR_FRUSTUM) && d > 0.0f && d < shot->near_z)
            r.note |= KILN_CAMLINT_NOTE_NEAR_AIM;

        if (bounds && bounds->valid && r.outside_key < 0) {
            for (int a = 0; a < 3; a++) {
                if (k[i].eye.v[a] < bounds->mins.v[a] ||
                    k[i].eye.v[a] > bounds->maxs.v[a]) {
                    r.note |= KILN_CAMLINT_NOTE_OUTSIDE;
                    r.outside_key = i;
                    break;
                }
            }
        }
    }

    if (k[0].t > 1e-3f) r.note |= KILN_CAMLINT_NOTE_LATE_START;

    r.tail = shot->duration - k[n - 1].t;
    if (r.tail > shot->duration * KILN_CAMLINT_TAIL_FRAC)
        r.note |= KILN_CAMLINT_NOTE_DEAD_TAIL;

    // Time order has to be sound before the measurements below mean anything —
    // a negative span produces a negative speed and a garbage curve.
    if (r.err & (KILN_CAMLINT_ERR_TIME_ORDER | KILN_CAMLINT_ERR_NAN)) {
        if (out) *out = r;
        return r.err;
    }

    // ── Per segment: speed, and the flown curve ────────────────────────
    int moving_segs = 0;
    for (int i = 0; i + 1 < n; i++) {
        const float span = k[i + 1].t - k[i].t;
        const float chord = v_dist(k[i].eye, k[i + 1].eye);
        if (span <= 0.0f) continue;

        // A HOLD is deliberate — the intake holds on the sit/type beat and on
        // the empty bed — so a zero-length segment must not drag speed_min to
        // zero and report every shot with a pause as a hitch. Only segments
        // that actually move are compared.
        if (chord > 0.0f) {
            const float sp = chord / span;
            if (sp > r.speed_max) r.speed_max = sp;
            if (moving_segs == 0 || sp < r.speed_min) r.speed_min = sp;
            moving_segs++;
        }

        if (chord <= 0.0f) continue;

        // The bulge. kiln_camkey_sample is the SAME function the runtime flies,
        // so this measures the real curve rather than one that resembles it.
        for (int s = 1; s < KILN_CAMLINT_SAMPLES_PER_SEG; s++) {
            const float f = (float)s / (float)KILN_CAMLINT_SAMPLES_PER_SEG;
            fm_vec3_t eye;
            kiln_camkey_sample(k, n, shot->loop, k[i].t + span * f, &eye, NULL);
            const float dev = dist_to_segment(eye, k[i].eye, k[i + 1].eye)
                              / chord;
            if (dev > r.overshoot) {
                r.overshoot = dev;
                r.overshoot_seg = i;
            }
        }
    }

    if (moving_segs > 1 && r.speed_min > 0.0f) {
        r.speed_ratio = r.speed_max / r.speed_min;
        if (r.speed_ratio > KILN_CAMLINT_HITCH_RATIO) r.note |= KILN_CAMLINT_NOTE_HITCH;
    }
    if (r.overshoot > KILN_CAMLINT_OVERSHOOT_FRAC) r.note |= KILN_CAMLINT_NOTE_OVERSHOOT;

    if (out) *out = r;
    return r.err;
}
