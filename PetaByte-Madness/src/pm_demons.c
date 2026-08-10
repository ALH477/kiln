// SPDX-License-Identifier: MPL-2.0
//
// pm_demons.c — the four demons. See pm_demons.h for the design and for
// the two places this deviates from docs/VEIL_DESIGN.md.

#include "pm_demons.h"

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>

#include <m64/m64_engine.h>
#include <m64/m64_gui.h>
#include <m64/m64_clip.h>
#include <m64/m64_event.h>

#include <string.h>

// ── Per-species tuning ─────────────────────────────────────────────────
#define IMP_SPEED        62.0f
#define HELLHOUND_SPEED  95.0f   // fast, but only while it is unobserved
#define GARGOYLE_SPEED   70.0f
#define OVERLORD_SPEED   34.0f

#define OVERLORD_HALO    260.0f  // radius inside which the veil is pinned on
#define SIGHT_RANGE      520.0f
#define EYE_HEIGHT       22.0f   // where a demon's eyes sit above its origin

// ── Shared per-instance state ──────────────────────────────────────────
// One struct for all four species: they differ in behaviour, not in what
// they need to remember, and a single 24-byte block keeps every profile
// comfortably inside M64_ACTOR_STATE_MAX (64) with room for the state a
// real combat pass will add.
typedef struct {
    fm_vec3_t home;     // spawn point — the statue's plinth, the patrol anchor
    uint8_t   noticed;  // has fired PM_EV_DEMON_NOTICE since it was last hidden
    uint8_t   sees;     // had line of sight to the player this frame
    uint8_t   _pad[2];
} PMDemonState;

// ── Frame-scoped bindings ──────────────────────────────────────────────
static const PMVeil *g_veil;
static fm_vec3_t     g_player_eye;
static int           g_player_seen;

// One model per species, indexed by profile id. NULL is survivable.
static T3DModel *g_models[PM_PROFILE_DEMON_COUNT];

void pm_demons_bind(const PMVeil *veil, fm_vec3_t player_eye)
{
    g_veil = veil;
    g_player_eye = player_eye;
    g_player_seen = 0;  // recomputed by this frame's updates
}

int pm_demons_player_seen(void) { return g_player_seen; }

// ── Helpers ────────────────────────────────────────────────────────────
static inline fm_vec3_t sub3(fm_vec3_t a, fm_vec3_t b)
{
    return (fm_vec3_t){{ a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2] }};
}

static fm_vec3_t demon_eye(const M64Actor *a)
{
    fm_vec3_t e = a->xform.pos;
    e.v[1] += EYE_HEIGHT;
    return e;
}

/** Line of sight, and the reciprocity rule in one function: a demon can
 *  only see the player while the veil is up (§6 — observation goes both
 *  ways), it must be in range, and nothing solid may be between them. */
static int can_see_player(const M64Actor *a)
{
    if (!g_veil || g_veil->step == 0) return 0;

    fm_vec3_t eye = demon_eye(a);
    fm_vec3_t d = sub3(g_player_eye, eye);
    if (fm_vec3_len(&d) > SIGHT_RANGE) return 0;

    M64Trace tr = m64_clip_ray(eye, g_player_eye);
    return tr.fraction >= 1.0f;
}

/** Walk toward the player, sliding along whatever the corridor puts in the
 *  way. Horizontal only — nothing in this slice flies or falls. */
static void chase(M64Actor *a, float speed, float dt)
{
    fm_vec3_t d = sub3(g_player_eye, a->xform.pos);
    d.v[1] = 0.0f;
    float len = fm_vec3_len(&d);
    if (len < 24.0f) return;  // close enough; attacks land in the combat pass

    const fm_vec3_t vel = {{ d.v[0] / len * speed, 0.0f, d.v[2] / len * speed }};
    const fm_vec3_t mins = {{ -10.0f, 0.0f, -10.0f }};
    const fm_vec3_t maxs = {{  10.0f, 40.0f,  10.0f }};

    a->xform.pos = m64_clip_slide(a->xform.pos, (fm_vec3_t){{ vel.v[0] * dt,
                                                              vel.v[1] * dt,
                                                              vel.v[2] * dt }},
                                  mins, maxs, 3);
    a->xform.rot_axis = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
    a->xform.rot_angle = fm_atan2f(d.v[0], d.v[2]);
}

/** Fire PM_DEMON_NOTICE exactly once per reveal — on the frame a demon
 *  first gets sight of the player, and never again until it loses it. A
 *  per-frame "I can see you" sting would be unlistenable. */
static void notice_edge(M64Actor *a, PMDemonState *s)
{
    if (s->sees && !s->noticed) {
        s->noticed = 1;
        m64_event_post(m64_actor_handle_of(a), PM_EV_DEMON_NOTICE, 0,
                       NULL, 0, 128);
    } else if (!s->sees) {
        s->noticed = 0;
    }
}

// ── Common init ────────────────────────────────────────────────────────
static void demon_init(M64Actor *self, const M64Dict *spawn_args)
{
    (void)spawn_args;
    PMDemonState *s = (PMDemonState *)self->state;
    s->home = self->xform.pos;
    s->noticed = 0;
    s->sees = 0;
    self->health = 3;
    self->xform.scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};
}

// ── IMP: the teaching enemy ────────────────────────────────────────────
// The first time you turn the veil on in a room you thought was empty,
// there are three of them and one is close. It knows exactly where you are
// the moment you resolve it, and not one frame before.
static void imp_update(M64Actor *self, float dt)
{
    PMDemonState *s = (PMDemonState *)self->state;
    s->sees = (uint8_t)can_see_player(self);
    if (s->sees) { g_player_seen = 1; chase(self, IMP_SPEED, dt); }
    notice_edge(self, s);
}

// ── HELLHOUND: the stalker ─────────────────────────────────────────────
// It only moves while the veil is DOWN. Veil up and it freezes exactly
// where it is — visible, motionless, nearer than it was. The player's
// instinct is to hold the veil up forever, which is what the air meter and
// the reciprocity rule exist to stop.
//
// Note it does not need line of sight to close: it hunts you in the dark,
// where you cannot see it. Sight only matters for the notice sting.
static void hellhound_update(M64Actor *self, float dt)
{
    PMDemonState *s = (PMDemonState *)self->state;
    s->sees = (uint8_t)can_see_player(self);
    if (s->sees) g_player_seen = 1;
    notice_edge(self, s);

    if (g_veil && g_veil->step == 0) chase(self, HELLHOUND_SPEED, dt);
    // Otherwise: nothing. Its `frozen` clip is deliberately almost nothing
    // — a shallow breath and a slow pulse in the eyes.
}

// ── GARGOYLE: the ambush ───────────────────────────────────────────────
// Because a phantom is not drawn at all with the veil down, a real statue
// prop can stand in the same spot. Veil down: a statue, identical to the
// forty others bolted around the level. Veil up: the statue is gone and a
// gargoyle is standing there. The level teaches you to stop trusting
// statuary, which is free level design falling out of a rendering
// property.
//
// It only ambushes what has looked at it — so it holds its perch until the
// notice edge has fired, and only then unfurls.
static void gargoyle_update(M64Actor *self, float dt)
{
    PMDemonState *s = (PMDemonState *)self->state;
    s->sees = (uint8_t)can_see_player(self);
    if (s->sees) g_player_seen = 1;
    const int was_noticed = s->noticed;
    notice_edge(self, s);

    if (was_noticed) chase(self, GARGOYLE_SPEED, dt);
}

// ── OVERLORD: the elite ────────────────────────────────────────────────
// Inside its halo the veil is pinned on and the player's toggle does
// nothing. Fight one alongside a hellhound and the design closes: the
// overlord holds the veil up, so the hellhound is frozen — until you kill
// the overlord, the crown collapses, the veil drops, and everything you
// froze starts moving at once.
static void overlord_update(M64Actor *self, float dt)
{
    PMDemonState *s = (PMDemonState *)self->state;
    s->sees = (uint8_t)can_see_player(self);
    if (s->sees) { g_player_seen = 1; chase(self, OVERLORD_SPEED, dt); }
    notice_edge(self, s);
}

float pm_demons_veil_force_at(fm_vec3_t pos)
{
    float strongest = 0.0f;
    for (M64Actor *a = m64_actor_first(M64_ACTOR_CAT_BOSS); a;
         a = m64_actor_next(a)) {
        if (a->profile_id != PM_PROFILE_OVERLORD) continue;
        fm_vec3_t d = sub3(pos, a->xform.pos);
        const float r = fm_vec3_len(&d);
        if (r >= OVERLORD_HALO) continue;
        // Ramps in over the outer half of the halo, so walking into an
        // overlord's range reads as the filter being dragged up rather than
        // snapping.
        float f = (OVERLORD_HALO - r) / (OVERLORD_HALO * 0.5f);
        if (f > 1.0f) f = 1.0f;
        if (f > strongest) strongest = f;
    }
    return strongest;
}

// ── Draw ───────────────────────────────────────────────────────────────
// The phantom rule, and the entire frame-time argument, is this one
// branch: with the veil down the body is not submitted, so a corridor can
// hold a dozen demons for free.
static void demon_draw(M64Actor *self)
{
    if (!pm_veil_demon_submit(g_veil)) return;

    T3DModel *m = g_models[self->profile_id];
    if (!m) return;

    m64_transform_push(&self->xform);
    t3d_model_draw(m);
    m64_transform_pop();
}

// ── Profiles ───────────────────────────────────────────────────────────
// Categories are chosen so the halo query and any future targeting sweep
// can walk one list: the three common demons are ENEMY, the overlord is
// BOSS.
static const M64ActorProfile g_profiles[PM_PROFILE_DEMON_COUNT] = {
    [PM_PROFILE_IMP] = {
        .name = "imp", .category = M64_ACTOR_CAT_ENEMY,
        .state_size = sizeof(PMDemonState),
        .init = demon_init, .update = imp_update, .draw = demon_draw,
    },
    [PM_PROFILE_HELLHOUND] = {
        .name = "hellhound", .category = M64_ACTOR_CAT_ENEMY,
        .state_size = sizeof(PMDemonState),
        .init = demon_init, .update = hellhound_update, .draw = demon_draw,
    },
    [PM_PROFILE_GARGOYLE] = {
        .name = "gargoyle", .category = M64_ACTOR_CAT_ENEMY,
        .state_size = sizeof(PMDemonState),
        .init = demon_init, .update = gargoyle_update, .draw = demon_draw,
    },
    [PM_PROFILE_OVERLORD] = {
        .name = "overlord", .category = M64_ACTOR_CAT_BOSS,
        .state_size = sizeof(PMDemonState),
        .init = demon_init, .update = overlord_update, .draw = demon_draw,
    },
};

const M64ActorProfile *pm_demons_profiles(void) { return g_profiles; }

void pm_demons_load(void)
{
    static const char *paths[PM_PROFILE_DEMON_COUNT] = {
        "rom:/models/imp.t3dm",
        "rom:/models/hellhound.t3dm",
        "rom:/models/gargoyle.t3dm",
        "rom:/models/overlord.t3dm",
    };
    for (int i = 0; i < PM_PROFILE_DEMON_COUNT; i++) {
        g_models[i] = t3d_model_load(paths[i]);
        if (!g_models[i]) debugf("pm_demons: missing %s\n", paths[i]);
    }
}

// ── Eyes ───────────────────────────────────────────────────────────────
// The stand-in described in pm_demons.h: projected screen-space dots, one
// occlusion ray each, instead of a VEIL_PAL_EYES material. Drawn for every
// demon regardless of veil state — that is the point of the eye exception.
// Veil down, they are the only thing on screen.
static void draw_eyes_for(const M64Scene *scene, uint8_t category)
{
    for (M64Actor *a = m64_actor_first(category); a; a = m64_actor_next(a)) {
        const fm_vec3_t eye = demon_eye(a);

        // One ray per demon, from the player's eye. Without this the
        // pinpricks shine through the bulkheads, which reads as a bug
        // rather than as dread.
        M64Trace tr = m64_clip_ray(g_player_eye, eye);
        if (tr.fraction < 1.0f) continue;

        int sx, sy;
        if (!m64_scene_project(scene, eye, PM_SCREEN_W, PM_SCREEN_H, &sx, &sy))
            continue;  // behind the camera
        if (sx < 0 || sy < 0 || sx >= PM_SCREEN_W || sy >= PM_SCREEN_H) continue;

        // Two dots, 3 px apart. Under the veil the demons already own the
        // top of the value range, so the eyes brighten with it rather than
        // competing when it is down.
        const uint8_t v = (uint8_t)(140 + (g_veil ? g_veil->step : 0) * 14);
        const color_t c = RGBA32(v, 0x18, 0x1E, 0xFF);
        m64_gui_rect(sx - 3, sy, 2, 2, c);
        m64_gui_rect(sx + 1, sy, 2, 2, c);
    }
}

void pm_demons_draw_eyes(const M64Scene *scene)
{
    draw_eyes_for(scene, M64_ACTOR_CAT_ENEMY);
    draw_eyes_for(scene, M64_ACTOR_CAT_BOSS);
}
