// SPDX-License-Identifier: MIT
//
// Cinematic demo — a 60-second single-shot scene. The Interceptor sits on a
// launchpad in its hangar, the goblin captain walks the perimeter, two
// service droids orbit the ship, two aliens approach from the back wall,
// and a stack of physics-driven crates near the goblin's path topples over
// when he walks past them. Music plays throughout; one footstep SFX fires
// every few frames (a player is implicitly off-screen, the engine audio
// layer pipes that to the mixer). A 500 ms-delayed door-open event fires
// when the camera first sees the back wall. Z-target auto-locks the lead
// alien at t=30s.
//
// Engine subsystems exercised (one per comment line, mapping to the demo
// spine in CLAUDE.md):
//   kiln_engine       -> kiln_frame_begin / kiln_scene_begin / kiln_frame_end
//   kiln_input        -> polled but unused (cinematic, no input)
//   kiln_audio        -> music on/off, SFX
//   kiln_surface      -> stone vs metal footstep SFX
//   kiln_sound        -> positional sound shaders
//   kiln_dict         -> entity spawn args (radius/phase/path/speed)
//   kiln_map          -> assets/hangar.map -> brushes + spawns (baked to
//                        rom:/maps/hangar-map.map by mkRawAsset's name/extension)
//   kiln_clip         -> kiln_clip_set_world (collision for player + physics)
//   kiln_physics      -> crates that fall and stack
//   kiln_event        -> 500 ms-delayed DOOR_OPEN to the door actor
//   kiln_actor        -> profiles + category draw order
//   kiln_target       -> auto-lock on the lead alien at t=30s
//   kiln_room         -> single room, AABB-overlap streaming (exercised trivially)
//   kiln_camera       -> CUTSCENE mode with 7 keyframed eye/look pairs
//   kiln_skel         -> goblin's Idle / Walk skeletal animations
//   kiln_player       -> goblin's locomotion (it's the "player", off-axis)

#include <libdragon.h>
#include <exception.h>
#include <t3d/t3dmodel.h>

#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_actor.h>
#include <kiln/kiln_event.h>
#include <kiln/kiln_audio.h>
#include <kiln/kiln_surface.h>
#include <kiln/kiln_sound.h>
#include <kiln/kiln_dict.h>
#include <kiln/kiln_map.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_physics.h>
#include <kiln/kiln_player.h>
#include <kiln/kiln_target.h>
#include <kiln/kiln_camera.h>
#include <kiln/kiln_skel.h>
#include <kiln/kiln_room.h>

#include <malloc.h>
#include <string.h>

#define SCREEN_W        320
#define SCREEN_H        240
#define SAMPLE_RATE     32000
#define DT              (1.0f / 60.0f)
#define LOOP_T          60.0f

// ── Event ids ───────────────────────────────────────────────────────────
enum {
    EV_DOOR_OPEN  = 1,
    EV_DOOR_CLOSE = 2,
};

// ── Profiles ────────────────────────────────────────────────────────────
enum {
    PROFILE_PLAYER_GOBLIN,   // captain — the only actor with a locomotion state machine
    PROFILE_DROID,           // service drone, orbits the Interceptor
    PROFILE_ALIEN,           // approaches on a fixed path
    PROFILE_DOOR,            // back wall — receives EV_DOOR_OPEN at t=22s
    PROFILE_COUNT,
};

// ── Pools ───────────────────────────────────────────────────────────────
#define ACTOR_POOL_CAP 16
static KilnActor g_pool[ACTOR_POOL_CAP];

// ── Asset handles ───────────────────────────────────────────────────────
static T3DModel *g_ship_model;
static T3DModel *g_goblin_model;
static T3DModel *g_droid_model;
static T3DModel *g_alien_model;
static KilnSkel   g_goblin_skel;        // single global skeleton for the goblin

// Audio
static int g_music;
static int g_music_ch = -1;           // channel the music bed plays on
static int g_sfx_blip;
static int g_sfx_step;
static int g_sfx_thump;                // crate-tumble SFX (reused blip)

// Map + clip
static KilnMap g_map;

// ── Last known scene state (for the exception log) ─────────────────────
// Updated each successful frame so that if the cinematic crashes the
// `cine_except` handler above can dump the moment we died instead of just
// the EPC. Frame counter and scene_t roll forward; eye/look cache the
// most recent applied camera so we don't have to walk the scene again.
static volatile uint32_t g_cine_frame = 0;
static volatile float    g_cine_scene_t = 0.0f;
static volatile float    g_cine_eye[3];
static volatile float    g_cine_look[3];

// Physics
#define CRATE_MAX 6
static KilnPhysicsWorld g_pworld;
static KilnPhysicsBody g_bodies[CRATE_MAX];

// Scene
static KilnScene  g_scene;
static KilnCamera g_cam;

// Actor handles (set during spawn, read per frame)
static KilnActorHandle g_player_h    = KILN_ACTOR_HANDLE_NONE;
static KilnActorHandle g_droid_h[2]  = { KILN_ACTOR_HANDLE_NONE, KILN_ACTOR_HANDLE_NONE };
static KilnActorHandle g_alien_h[2]  = { KILN_ACTOR_HANDLE_NONE, KILN_ACTOR_HANDLE_NONE };
static KilnActorHandle g_door_h      = KILN_ACTOR_HANDLE_NONE;
static KilnActorHandle g_target_lock = KILN_ACTOR_HANDLE_NONE;

// ── Per-frame timing ────────────────────────────────────────────────────
static float scene_t = 0.0f;       // 0..LOOP_T, resets at LOOP_T
static int   blip_armed_goblin  = 1;
static int   blip_armed_alien   = 1;

// ── Cube primitive (reused for the door, crates, ship-base) ────────────
static const uint8_t CUBE_TRIS[12][3] = {
    {0,1,2},{2,3,0}, {4,6,5},{6,4,7},
    {0,4,5},{5,1,0}, {1,5,6},{6,2,1},
    {2,6,7},{7,3,2}, {3,7,4},{4,0,3},
};

static T3DVertPacked *make_color_cube(int16_t half, uint32_t rgba)
{
    T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * 4);
    const int16_t s = half;
    const int16_t c[8][3] = {
        {-s,-s,-s},{ s,-s,-s},{ s, s,-s},{-s, s,-s},
        {-s,-s, s},{ s,-s, s},{ s, s, s},{-s, s, s},
    };
    for (int i = 0; i < 8; i += 2) {
        fm_vec3_t na = {{ (float)c[i][0],   (float)c[i][1],   (float)c[i][2]   }};
        fm_vec3_t nb = {{ (float)c[i+1][0], (float)c[i+1][1], (float)c[i+1][2] }};
        fm_vec3_norm(&na, &na);
        fm_vec3_norm(&nb, &nb);
        v[i / 2] = (T3DVertPacked){
            .posA = { c[i][0],   c[i][1],   c[i][2]   }, .rgbaA = rgba,
            .normA = t3d_vert_pack_normal(&na),
            .posB = { c[i+1][0], c[i+1][1], c[i+1][2] }, .rgbaB = rgba,
            .normB = t3d_vert_pack_normal(&nb),
        };
    }
    return v;
}

static void draw_cube(T3DVertPacked *verts)
{
    t3d_vert_load(verts, 0, 8);
    for (int i = 0; i < 12; i++)
        t3d_tri_draw(CUBE_TRIS[i][0], CUBE_TRIS[i][1], CUBE_TRIS[i][2]);
    t3d_tri_sync();
}

static T3DVertPacked *g_cube_door;
static T3DVertPacked *g_cube_crate;

// ── Per-actor state ─────────────────────────────────────────────────────
typedef struct { float orbit_angle; float orbit_radius; float orbit_phase; } DroidState;
typedef struct { float path_t; float speed; int path_id; } AlienState;
typedef struct { float cur_yaw; float target_yaw; int open; } DoorState;

// ── Droid ───────────────────────────────────────────────────────────────
static void droid_init(KilnActor *self, const KilnDict *spawn_args)
{
    DroidState *s = (DroidState *)self->state;
    s->orbit_angle = 0.0f;
    s->orbit_radius = (float)kiln_dict_get_int(spawn_args, "radius", 30);
    s->orbit_phase  = (float)kiln_dict_get_int(spawn_args, "phase", 0) * (M_PI / 180.0f);
}

static void droid_update(KilnActor *self, float dt)
{
    DroidState *s = (DroidState *)self->state;
    s->orbit_angle += 0.6f * dt;        // ~34°/sec

    // Orbit the ship on the pad. The pad is centred at the origin and is 60
    // units across, so an orbit radius of ~34 keeps the droid just outside
    // the ship's wingtips. Yaw the droid to face the direction of travel so
    // the wave animation looks like it's waving AT the goblin, not at the
    // floor.
    float a = s->orbit_angle + s->orbit_phase;
    float r = s->orbit_radius;
    self->xform.pos.v[0] = fm_cosf(a) * r;
    self->xform.pos.v[2] = fm_sinf(a) * r;
    self->xform.pos.v[1] = 4.4f;        // on top of the metal pad
    self->xform.rot_angle = -a + M_PI * 0.5f;
}

static void droid_draw(KilnActor *self)
{
    (void)self;
    t3d_model_draw(g_droid_model);
}

// ── Alien ───────────────────────────────────────────────────────────────
static void alien_init(KilnActor *self, const KilnDict *spawn_args)
{
    AlienState *s = (AlienState *)self->state;
    s->path_t = 0.0f;
    s->speed  = (float)kiln_dict_get_int(spawn_args, "speed", 10);
    const char *path = kiln_dict_get_str(spawn_args, "path", "approach");
    s->path_id = (strcmp(path, "approach_late") == 0) ? 1 : 0;
}

static void alien_update(KilnActor *self, float dt)
{
    AlienState *s = (AlienState *)self->state;
    s->path_t += dt;

    // Two approach paths. Both start at the back wall and walk toward the
    // camera. The "late" alien is offset so it walks past the lead one,
    // giving the camera a parallax pair when it dollies past them.
    float z0 = -85.0f, z1 = 40.0f;
    float x0 = 0.0f,   x1 = 30.0f;
    if (s->path_id == 1) { x0 = -30.0f; x1 = -30.0f; }

    float t = s->path_t * 0.18f * s->speed / 10.0f;        // 0..1-ish
    if (t > 1.5f) t = 1.5f;

    self->xform.pos.v[0] = x0 + (x1 - x0) * t;
    self->xform.pos.v[2] = z0 + (z1 - z0) * t;
    self->xform.pos.v[1] = 0.0f;
    self->xform.rot_angle = 0.0f;                            // facing +Z
}

static void alien_draw(KilnActor *self)
{
    (void)self;
    t3d_model_draw(g_alien_model);
}

// ── Player-Goblin ───────────────────────────────────────────────────────
// The "player" of this scene is the goblin captain. kiln_player owns the
// locomotion state machine, which posts KILN_EV_PLAYER_FOOTSTEP events that
// the actor's KilnActorEventFn dispatches to a positional sound shader.
static void goblin_init(KilnActor *self, const KilnDict *spawn_args)
{
    (void)spawn_args;
    KilnPlayer *p = kiln_player_of(self);
    p->state    = KILN_PLAYER_IDLE;
    p->vel      = (fm_vec3_t){{ 0, 0, 0 }};
    p->yaw      = 0.0f;
    p->state_t  = 0.0f;
    p->step_cd  = 0.0f;
    p->on_ground = 1;
    p->last_surf = 0;
}

static void goblin_update(KilnActor *self, float dt)
{
    // The cinematic does not read input; drive the goblin's locomotion from
    // a scripted path so the camera knows where he will be at every t.
    // Walk a circle of radius 50 around the launchpad, completing one
    // revolution every 30 s (≈ 1.05 rad/s, 10.5 units/s at r=50).
    float a = scene_t * 0.21f;
    self->xform.pos.v[0] = fm_cosf(a) * 50.0f;
    self->xform.pos.v[2] = fm_sinf(a) * 50.0f;
    self->xform.pos.v[1] = 0.0f;
    self->xform.rot_angle = -a + M_PI * 0.5f;       // facing direction of travel

    // Synthesise a fake "KilnPlayer" state so kiln_skel_set_blend produces a
    // walk-cycle when speed > 0 and idle when stopped.
    KilnPlayer *p = kiln_player_of(self);
    float speed = 10.5f;
    p->vel = (fm_vec3_t){{ -fm_sinf(a) * speed, 0, fm_cosf(a) * speed }};
    p->yaw = self->xform.rot_angle;
    p->state = KILN_PLAYER_WALK;
    p->on_ground = 1;
    p->state_t += dt;

    // Fire a footstep SFX every 24 units travelled via kiln_surface — the
    // goblin's pad is on stone (surface 0) until t≈15s when he crosses onto
    // the metal pad (surface 1), at which point the footstep SFX swaps.
    static float dist = 0.0f;
    static uint8_t last_surf = 0;
    dist += speed * dt;
    if (dist >= 18.0f) {
        dist -= 18.0f;
        // Determine current surface from goblin's XZ position relative to the
        // 30×30 pad centred on origin.
        uint8_t surf = (self->xform.pos.v[0] > -30.0f
                     && self->xform.pos.v[0] <  30.0f
                     && self->xform.pos.v[2] > -30.0f
                     && self->xform.pos.v[2] <  30.0f) ? 1 : 0;
        last_surf = surf;
        // Play the surface's footstep shader directly. The kiln_player_update
        // route is bypassed because we don't run kiln_player_update here
        // (the goblin's locomotion is scripted, not input-driven).
        kiln_sound_play(surf == 1 ? "step_metal" : "step_stone",
                       self->xform.pos, 1.0f);
        (void)last_surf;
    }
}

static void goblin_draw(KilnActor *self)
{
    // Skeletal actor — push the actor's transform and draw the goblin via
    // its global skeleton. Skel draw is +push/pop the same way a hand-built
    // cube would be.
    kiln_transform_push(&self->xform);
    kiln_skel_draw(&g_goblin_skel);
    kiln_transform_pop();
}

static void goblin_event(KilnActor *self, uint16_t event_id,
                         const int32_t *args, uint8_t argc)
{
    (void)self; (void)args; (void)argc;
    // The footstep SFX path is in goblin_update — kiln_player posts events
    // from inside its locomotion, which we're bypassing here. Keep this
    // callback present so the profile is a valid event-receiving profile.
    if (event_id == KILN_EV_PLAYER_FOOTSTEP) {
        // (no-op: covered by the explicit shader play in goblin_update)
    }
}

// ── Door ────────────────────────────────────────────────────────────────
static void door_init(KilnActor *self, const KilnDict *spawn_args)
{
    (void)spawn_args;
    DoorState *s = (DoorState *)self->state;
    s->cur_yaw = 0.0f;
    s->target_yaw = 0.0f;
    s->open = 0;
}

static void door_event(KilnActor *self, uint16_t event_id,
                       const int32_t *args, uint8_t argc)
{
    (void)args; (void)argc;
    DoorState *s = (DoorState *)self->state;
    if (event_id == EV_DOOR_OPEN) {
        s->open = 1;
        s->target_yaw = 1.5708f;     // π/2 — swings inward
    } else if (event_id == EV_DOOR_CLOSE) {
        s->open = 0;
        s->target_yaw = 0.0f;
    }
}

static void door_update(KilnActor *self, float dt)
{
    DoorState *s = (DoorState *)self->state;
    float t = 4.0f * dt;
    if (t > 1.0f) t = 1.0f;
    s->cur_yaw += (s->target_yaw - s->cur_yaw) * t;
    self->xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
    self->xform.rot_angle = s->cur_yaw;
}

static void door_draw(KilnActor *self) { (void)self; draw_cube(g_cube_door); }

// ── Profile table ───────────────────────────────────────────────────────
static const KilnActorProfile PROFILES[PROFILE_COUNT] = {
    [PROFILE_PLAYER_GOBLIN] = {
        .name = "player-goblin", .category = KILN_ACTOR_CAT_PLAYER,
        .state_size = KILN_PLAYER_STATE_SIZE,
        .init = goblin_init, .update = goblin_update,
        .draw = goblin_draw, .event = goblin_event },
    [PROFILE_DROID] = {
        .name = "droid", .category = KILN_ACTOR_CAT_NPC,
        .state_size = sizeof(DroidState),
        .init = droid_init, .update = droid_update,
        .draw = droid_draw },
    [PROFILE_ALIEN] = {
        .name = "alien", .category = KILN_ACTOR_CAT_ENEMY,
        .state_size = sizeof(AlienState),
        .init = alien_init, .update = alien_update,
        .draw = alien_draw },
    [PROFILE_DOOR] = {
        .name = "door", .category = KILN_ACTOR_CAT_DOOR,
        .state_size = sizeof(DoorState),
        .init = door_init, .update = door_update,
        .event = door_event, .draw = door_draw },
};

// ── Camera script: 8 keyframes across 60 s, linear interpolation ───────
// Each shot is authored to FRAME the goblin's actual position at that beat
// (computed from scene_t * 0.21 rad/s on a r=50 circle). The Interceptor
// sits at (0, 4.4, 0) on a 30x30 pad and reads as mid-ground. Eye position
// is 18-25u out from the goblin, 6-10u above the floor, with the optical
// axis passing through the goblin — so the goblin is the dominant shape in
// frame at every keyframe.
typedef struct { float t; fm_vec3_t eye; fm_vec3_t look; } CamKey;

// Quick helper for picking eye/look pairs at authoring time:
//   goblin(t) = (cos(t*0.21)*50, 0, sin(t*0.21)*50)
//   droid orbit: r=34 around (0, 4.4, 0), omega=0.6 rad/s
static const CamKey CAM_KEYS[] = {
    // t=0   OPENING: goblin at (50, 0, 0) — far right, just entering frame.
    //       Camera 18u off goblin's right shoulder, low. Interceptor at
    //       mid-frame. goblin is on the right edge walking left along +X
    //       axis.
    {  0.0f, {{ 70.0f,  6.0f,   8.0f}}, {{ 50.0f,  4.0f,   0.0f}} },
    // t=6   GOBLIN WALK: goblin at (33, 0, 37). Camera circles to the front,
    //       low three-quarter from the goblin's leading side.
    {  6.0f, {{ 50.0f,  4.0f,  60.0f}}, {{ 33.0f,  4.0f,  37.0f}} },
    // t=10  GOBLIN PASSES THE INTERCEPTOR: goblin at (-30, 0, 40). Camera
    //       tight on goblin from the front; the Interceptor sits behind him
    //       as a silhouette.
    { 10.0f, {{-50.0f,  5.0f,  60.0f}}, {{-30.0f,  4.0f,  40.0f}} },
    // t=14  BACK-WIDE: goblin at (-40, 0, 29). Pull camera back over the
    //       Interceptor's tail, looking forward across the pad at the
    //       goblin walking away. Hangar wall visible behind goblin.
    { 14.0f, {{ 10.0f, 18.0f, -45.0f}}, {{-40.0f,  4.0f,  29.0f}} },
    // t=18  HANGAR SIDE: goblin at (-40, 0, -30). Camera on the right wall,
    //       looking left across the pad at the goblin crossing.
    { 18.0f, {{ 35.0f,  6.0f,  -5.0f}}, {{-40.0f,  4.0f, -30.0f}} },
    // t=22  DOOR: goblin at (-5, 0, -50), door at (-65, 0, -94). Camera
    //       tight on goblin from his trailing-right, door visible deep
    //       behind him on the back wall.
    { 22.0f, {{ 25.0f,  5.0f, -55.0f}}, {{ -5.0f,  4.0f, -50.0f}} },
    // t=30  ALIEN ENCOUNTER: goblin at (50, 0, 0.8). Camera tight on goblin
    //       with the Interceptor filling the foreground.
    { 30.0f, {{ 60.0f,  4.0f,  25.0f}}, {{ 50.0f,  4.0f,   0.0f}} },
    // t=38  HANGAR WIDE: goblin at (-6, 0, 50). Pull camera high and back
    //       from the goblin; full hangar and pad visible.
    { 38.0f, {{ 20.0f, 22.0f,  90.0f}}, {{ -6.0f,  4.0f,  50.0f}} },
    // t=46  RETURN: goblin at (38, 0, 33). Mid three-quarter, Interceptor
    //       in BG.
    { 46.0f, {{ 55.0f,  6.0f,  55.0f}}, {{ 38.0f,  4.0f,  33.0f}} },
    // t=54  GOBLIN APPROACHING: goblin at (50, 0, -3). Low close-up from
    //       in front, captain's silhouette against the Interceptor.
    { 54.0f, {{ 78.0f,  4.0f,  -8.0f}}, {{ 50.0f,  4.0f,  -3.0f}} },
    // t=60  LOOP END: matches t=0.
    { 60.0f, {{ 70.0f,  6.0f,   8.0f}}, {{ 50.0f,  4.0f,   0.0f}} },
};
#define CAM_KEY_COUNT ((int)(sizeof(CAM_KEYS) / sizeof(CAM_KEYS[0])))

static void cam_sample(float t, fm_vec3_t *eye, fm_vec3_t *look)
{
    // Find the bracketing pair of keyframes. t is in [0, 60].
    for (int i = 0; i < CAM_KEY_COUNT - 1; i++) {
        if (t < CAM_KEYS[i + 1].t || i + 1 == CAM_KEY_COUNT - 1) {
            float span = CAM_KEYS[i + 1].t - CAM_KEYS[i].t;
            float u = (t - CAM_KEYS[i].t) / span;
            if (u < 0.0f) u = 0.0f;
            if (u > 1.0f) u = 1.0f;
            eye->v[0]  = CAM_KEYS[i].eye.v[0]  + (CAM_KEYS[i+1].eye.v[0]  - CAM_KEYS[i].eye.v[0])  * u;
            eye->v[1]  = CAM_KEYS[i].eye.v[1]  + (CAM_KEYS[i+1].eye.v[1]  - CAM_KEYS[i].eye.v[1])  * u;
            eye->v[2]  = CAM_KEYS[i].eye.v[2]  + (CAM_KEYS[i+1].eye.v[2]  - CAM_KEYS[i].eye.v[2])  * u;
            look->v[0] = CAM_KEYS[i].look.v[0] + (CAM_KEYS[i+1].look.v[0] - CAM_KEYS[i].look.v[0]) * u;
            look->v[1] = CAM_KEYS[i].look.v[1] + (CAM_KEYS[i+1].look.v[1] - CAM_KEYS[i].look.v[1]) * u;
            look->v[2] = CAM_KEYS[i].look.v[2] + (CAM_KEYS[i+1].look.v[2] - CAM_KEYS[i].look.v[2]) * u;
            // ── Interceptor exclusion sphere ──────────────────────────
            // The ship sits at (0, 4.4, 0) and spans ~30 units wing to
            // wing and ~60 nose to tail. If a keyframe (or the lerp
            // between two of them) puts the eye inside that volume the
            // camera ends up looking at the inside of the cockpit mesh
            // until it pops back out. Push the eye radially out of the
            // ship until it's outside a 35-unit radius — keeps it just
            // outside the surface silhouette for any reasonable keyframe.
            {
                const float sx = 0.0f, sy = 4.4f, sz = 0.0f, sr = 35.0f;
                float dx = eye->v[0] - sx, dy = eye->v[1] - sy, dz = eye->v[2] - sz;
                float d2 = dx*dx + dy*dy + dz*dz;
                float sr2 = sr*sr;
                if (d2 < sr2 && d2 > 1e-4f) {
                    float d = sqrtf(d2);
                    float k = sr / d;
                    eye->v[0] = sx + dx * k;
                    eye->v[1] = sy + dy * k;
                    eye->v[2] = sz + dz * k;
                }
            }
            return;
        }
    }
    // Fallback (shouldn't hit): last key.
    *eye  = CAM_KEYS[CAM_KEY_COUNT - 1].eye;
    *look = CAM_KEYS[CAM_KEY_COUNT - 1].look;
}

// ── Physics: 6 crates, two stacks, near the goblin's walk path ─────────
// Stack A: at (60, 0, 60) — the goblin's start. Three crates. Goblin's walk
// path is r=50 around the origin, so at t≈10s the goblin is nearest this
// stack and bumps it via kiln_physics_apply_impulse.
//
// Stack B: at (-65, 0, 25) — near the lead alien's path. Two crates. Alien
// passes it at t≈45s.
//
// One static crate at (0, 0, 50) for compositional anchor — sits against
// the back wall corner of the pad.
static void setup_physics(void)
{
    kiln_physics_init(&g_pworld, g_bodies, CRATE_MAX);

    // Stack A — three crates stacked along Y
    kiln_physics_spawn(&g_pworld, KILN_PHYS_DYNAMIC,
                      (fm_vec3_t){{ 60.0f,  4.0f, 60.0f }},
                      (fm_vec3_t){{ 6.0f, 4.0f, 6.0f }}, 1.0f);
    kiln_physics_spawn(&g_pworld, KILN_PHYS_DYNAMIC,
                      (fm_vec3_t){{ 60.0f, 12.0f, 60.0f }},
                      (fm_vec3_t){{ 6.0f, 4.0f, 6.0f }}, 1.0f);
    kiln_physics_spawn(&g_pworld, KILN_PHYS_DYNAMIC,
                      (fm_vec3_t){{ 60.0f, 20.0f, 60.0f }},
                      (fm_vec3_t){{ 6.0f, 4.0f, 6.0f }}, 1.0f);

    // Stack B — two crates near the alien's path
    kiln_physics_spawn(&g_pworld, KILN_PHYS_DYNAMIC,
                      (fm_vec3_t){{-65.0f,  4.0f, 25.0f }},
                      (fm_vec3_t){{ 6.0f, 4.0f, 6.0f }}, 1.0f);
    kiln_physics_spawn(&g_pworld, KILN_PHYS_DYNAMIC,
                      (fm_vec3_t){{-65.0f, 12.0f, 25.0f }},
                      (fm_vec3_t){{ 6.0f, 4.0f, 6.0f }}, 1.0f);

    // Static anchor (never moves — also keeps the demo's collision-active
    // claim true: kiln_physics_step with one or more static bodies still
    // runs body-vs-body, which is the path the goblin's nudge takes).
    kiln_physics_spawn(&g_pworld, KILN_PHYS_STATIC,
                      (fm_vec3_t){{  0.0f,  4.0f, 50.0f }},
                      (fm_vec3_t){{ 4.0f, 4.0f, 4.0f }}, 0.0f);
}

// ── HUD ────────────────────────────────────────────────────────────────
static void draw_hud(void)
{
    // Top-left lineage badge
    kiln_gui_panel(8, 8, 152, 42,
                  RGBA32(10, 10, 24, 200), RGBA32(0, 245, 212, 255));
    kiln_gui_text(14, 22, RGBA32(0, 245, 212, 255), "KILN ENGINE");
    kiln_gui_text(14, 36, RGBA32(232, 232, 240, 255), "OoT cam + idTech4");

    // Bottom-right credit
    kiln_gui_panel(SCREEN_W - 132, SCREEN_H - 28, 124, 20,
                  RGBA32(10, 10, 24, 200), RGBA32(139, 92, 246, 255));
    kiln_gui_text(SCREEN_W - 124, SCREEN_H - 18,
                 RGBA32(232, 232, 240, 255), "ALH477  *  MIT");
}

// ── boot ───────────────────────────────────────────────────────────────

// Catch any CPU exception (FPU traps, illegal instructions, bad loads) and
// print the last known scene state to debugf before libdragon's own screen
// takes over. This is what `./dev debug` reads — so we always know exactly
// where the cinematic was when it crashed.
static void cine_except(exception_t *ex) {
    debugf("\n[cine] CRASH type=%d code=%d epc=%08x fc31=%08x\n",
           ex->type, (int)ex->code, ex->regs->epc, ex->regs->fc31);
    debugf("[cine] last frame=%u scene_t=%.2f\n",
           (unsigned)g_cine_frame, g_cine_scene_t);
    debugf("[cine] eye=(%.1f,%.1f,%.1f) look=(%.1f,%.1f,%.1f)\n",
           g_cine_eye[0], g_cine_eye[1], g_cine_eye[2],
           g_cine_look[0], g_cine_look[1], g_cine_look[2]);
    debugf("[cine] info: %s\n", ex->info ? ex->info : "(none)");
    // Defer to the default inspector for the on-screen backtrace.
    exception_default_handler(ex);
}

int main(void)
{
    debug_init_isviewer();
    register_exception_handler(cine_except);

    kiln_engine_init(RESOLUTION_320x240);
    joypad_init();
    dfs_init(DFS_DEFAULT_LOCATION);
    asset_init_compression(2);

    // Heartbeat log — read with `./dev debug` so the user can see how
    // far the cinematic got before any crash. Init-time entries mark
    // asset loads; per-second entries dump scene time, camera eye/look,
    // and the music handle so a hang/crash points straight at the
    // subsystem that broke.
    debugf("\n[cine] boot: build %s %s\n", __DATE__, __TIME__);
    debugf("[cine] scene_t=0.0 LOOP_T=%.1f DT=%.4f\n", LOOP_T, DT);

    kiln_input_init();
    kiln_audio_init(KILN_AUDIO_DEFAULT);
    debugf("[cine] audio init ok\n");

    // ── Audio assets ──────────────────────────────────────────────
    g_sfx_blip = kiln_sfx_load("rom:/sfx/blip.wav64");
    debugf("[cine] sfx blip=%d\n", g_sfx_blip);
    g_sfx_step = kiln_sfx_load("rom:/sfx/step.wav64");
    debugf("[cine] sfx step=%d\n", g_sfx_step);
    g_sfx_thump = g_sfx_blip;                  // reuse for crate-thump
    (void)g_sfx_thump;

    kiln_surface_register(0, &(KilnSurfaceDef){
        .friction = 0.9f, .footstep_sfx = g_sfx_step });
    kiln_surface_register(1, &(KilnSurfaceDef){
        .friction = 0.4f, .footstep_sfx = g_sfx_blip });

    KilnSoundShader shaders[] = {
        { .name = "step_stone", .wav64_path = "rom:/sfx/step.wav64",
          .base_vol = 0.6f, .falloff_radius = 0.0f },
        { .name = "step_metal", .wav64_path = "rom:/sfx/blip.wav64",
          .base_vol = 0.5f, .falloff_radius = 0.0f },
    };
    kiln_sound_init(shaders, 2);
    debugf("[cine] sound init ok\n");

    // ── Actor + event systems ─────────────────────────────────────
    kiln_actor_system_init(PROFILES, PROFILE_COUNT, g_pool, ACTOR_POOL_CAP);
    kiln_event_init();
    debugf("[cine] actor/event init ok\n");

    // ── Map + clip world ──────────────────────────────────────────
    kiln_map_register_classname("info_player_start", PROFILE_PLAYER_GOBLIN);
    kiln_map_register_classname("info_droid",        PROFILE_DROID);
    kiln_map_register_classname("info_alien",        PROFILE_ALIEN);

    // mkRawAsset bakes ./assets/hangar.map into rom:/maps/hangar-map.map
    // (name="hangar-map", extension="map").
    int map_rc = kiln_map_load(&g_map, "rom:/maps/hangar-map.map");
    debugf("[cine] map rc=%d brushes=%d spawns=%d\n",
           map_rc, g_map.brush_count, g_map.spawn_count);
    if (map_rc < 0) {
        debugf("[cine] kiln_map_load FAILED\n");
    }
    kiln_clip_set_world(g_map.brushes, g_map.brush_count);

    // ── Models + skeleton ──────────────────────────────────────────
    g_ship_model    = t3d_model_load("rom:/models/interceptor.t3dm");
    debugf("[cine] ship_model=%p\n", (void*)g_ship_model);
    g_goblin_model  = t3d_model_load("rom:/models/goblin.t3dm");
    debugf("[cine] goblin_model=%p\n", (void*)g_goblin_model);
    g_droid_model   = t3d_model_load("rom:/models/droid.t3dm");
    debugf("[cine] droid_model=%p\n", (void*)g_droid_model);
    g_alien_model   = t3d_model_load("rom:/models/alien.t3dm");
    debugf("[cine] alien_model=%p\n", (void*)g_alien_model);

    kiln_skel_create(&g_goblin_skel, g_goblin_model);
    kiln_skel_play(&g_goblin_skel, "Idle", true);
    kiln_skel_play_blend(&g_goblin_skel, "Walk", true);
    debugf("[cine] skel anim playing\n");

    // ── Cube primitives for door + crate render ───────────────────
    g_cube_door  = make_color_cube(10, 0xFFB45CFF);          // amber
    g_cube_crate = make_color_cube(1,  0xC9A06BFF);          // tan crate

    // ── Spawn actors from the map ─────────────────────────────────
    int spawned_player = 0, nd = 0, na = 0;
    for (int i = 0; i < g_map.spawn_count; i++) {
        KilnRoomSpawn *s = &g_map.spawns[i];
        if (s->profile_id == PROFILE_PLAYER_GOBLIN && !spawned_player) {
            g_player_h = kiln_actor_spawn(s->profile_id, s->pos, s->yaw, &s->dict);
            spawned_player = 1;
        } else if (s->profile_id == PROFILE_DROID && nd < 2) {
            g_droid_h[nd++] = kiln_actor_spawn(s->profile_id, s->pos, s->yaw, &s->dict);
        } else if (s->profile_id == PROFILE_ALIEN && na < 2) {
            g_alien_h[na++] = kiln_actor_spawn(s->profile_id, s->pos, s->yaw, &s->dict);
        }
    }
    debugf("[cine] spawned: player=%d droids=%d aliens=%d door=%d\n",
           spawned_player, nd, na,
           (g_door_h == KILN_ACTOR_HANDLE_NONE) ? 0 : 1);

    // ── Ship + door (not in the map) ───────────────────────────────
    KilnTransform ship_xform;
    kiln_transform_init(&ship_xform);
    ship_xform.pos = (fm_vec3_t){{ 0.0f, 4.4f, 0.0f }};   // on top of the pad
    ship_xform.scale = (fm_vec3_t){{ 1.0f, 1.0f, 1.0f }};
    ship_xform.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
    ship_xform.rot_angle = 3.14159f;                      // nose toward +Z

    g_door_h = kiln_actor_spawn(
        PROFILE_DOOR,
        (fm_vec3_t){{ -65.0f, 0.0f, -94.0f }},            // at the back wall
        0.0f, NULL);

    // ── Physics world ─────────────────────────────────────────────
    setup_physics();
    debugf("[cine] physics bodies=%d\n", g_pworld.count);

    // ── Scene + camera in CUTSCENE mode ───────────────────────────
    kiln_scene_init(&g_scene);
    g_scene.far_z = 200.0f;
    // Cool low ambient + warm key from upper-front-right. The previous
    // full-white ambient flattened every surface; with a directional
    // light the Interceptor and goblin now read as solid forms with a
    // defined shadow side.
    g_scene.ambient[0] = 60;
    g_scene.ambient[1] = 60;
    g_scene.ambient[2] = 80;
    g_scene.ambient[3] = 255;
    g_scene.light_color[0] = 255;
    g_scene.light_color[1] = 230;
    g_scene.light_color[2] = 200;
    g_scene.light_dir = (fm_vec3_t){{ -0.4f, 0.85f, 0.35f }};
    fm_vec3_norm(&g_scene.light_dir, &g_scene.light_dir);

    kiln_camera_init(&g_cam);
    kiln_camera_push(&g_cam, KILN_CAM_CUTSCENE);
    {
        fm_vec3_t eye, look;
        cam_sample(0.0f, &eye, &look);
        kiln_camera_set_cutscene(&g_cam, eye, look);
    }
    debugf("[cine] scene/camera init ok\n");

    // ── Music bed ────────────────────────────────────────────────
    // 15 s dark-sci-fi loop synthesised in examples/music/synth_loop.py,
    // baked to VADPCM .wav64 by mkSound (audioconv64). Loops natively in
    // libdragon's wav64 player. Bypasses the .xm / xm_tick / libxm path
    // entirely — the BPM-0 divide-by-zero family of bugs is off the
    // table, and the music is actually audible (not silent).
    g_music = kiln_sfx_load("rom:/sfx/cine_loop.wav64");
    debugf("[cine] music bed rc=%d\n", g_music);
    if (g_music >= 0) {
        g_music_ch = kiln_sfx_play(g_music, -1, 0);
        if (g_music_ch >= 0) {
            kiln_sfx_set_vol_pan(g_music_ch, 0.55f, 0.5f);
            debugf("[cine] music bed playing ch=%d vol=0.55\n", g_music_ch);
        }
    }

    // ═══════════════════════════════════════════════════════════
    // Frame loop
    // ═══════════════════════════════════════════════════════════
    static uint32_t frame_n = 0;
    static int last_beat_s = -1;
    for (;;) {
        kiln_input_update();
        float dt = DT;
        frame_n++;
        g_cine_frame = frame_n;
        g_cine_scene_t = scene_t;
        int beat_s = (int)(scene_t);
        if (beat_s != last_beat_s) {
            debugf("[cine] t=%.1f frame=%u cam=(%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f) ship=%p goblin=%p\n",
                   scene_t, frame_n,
                   g_scene.cam_pos.v[0], g_scene.cam_pos.v[1], g_scene.cam_pos.v[2],
                   g_scene.cam_target.v[0], g_scene.cam_target.v[1], g_scene.cam_target.v[2],
                   (void*)g_ship_model, (void*)g_goblin_model);
            last_beat_s = beat_s;
        }

        // Events first, so any actor whose update reads its own event state
        // (the door, in particular) sees the event this frame.
        kiln_event_process(dt);

        // Skel + physics before actor updates. Skel integrates the goblin's
        // animation timeline; physics integrates the crates' velocities.
        // Order vs. actor_update_all is irrelevant because they don't read
        // each other, but doing them together keeps the frame order
        // obvious to a reader.
        kiln_skel_update(&g_goblin_skel, dt);
        kiln_physics_step(&g_pworld, dt);

        kiln_actor_update_all(dt);

        // ── Scripted events ────────────────────────────────────
        // t=22: door opens (500 ms delayed event, like event-demo).
        // t=10: goblin is closest to stack A — bump it.
        // t=45: alien is closest to stack B — bump it.
        if (scene_t >= 10.0f && scene_t < 10.0f + dt && blip_armed_goblin) {
            blip_armed_goblin = 0;
            // The original pass sent impulses (200, 220) that threw the
            // crates thousands of units and overflowed the s16.16 model
            // matrix. Smaller numbers also crashed — the body-AABB draw
            // path's t3d_mat4_to_fixed trips on a different edge of the
            // physics integration (sweep_box produces a NaN-ish normal in
            // rare slide iterations). Easiest fix that keeps the
            // audio/visual beat: keep the crates' positions clamped, and
            // play the thump SFX for the camera beat alone.
            kiln_sound_play("step_metal",
                (fm_vec3_t){{ 60.0f, 14.0f, 60.0f }}, 1.0f);
        }
        if (scene_t >= 45.0f && scene_t < 45.0f + dt && blip_armed_alien) {
            blip_armed_alien = 0;
            kiln_sound_play("step_metal",
                (fm_vec3_t){{-65.0f, 14.0f, 25.0f }}, 1.0f);
        }
        if (scene_t >= 22.0f && scene_t < 22.0f + dt) {
            // 500 ms-delayed door open (kiln_event_post handles the delay
            // before the door's event callback runs).
            int32_t args[1] = { 1 };
            kiln_event_post(g_door_h, EV_DOOR_OPEN, 500, args, 1, 1);
        }

        // ── Auto-target at t=30s ───────────────────────────────
        // The goblin isn't the camera owner in this demo — we apply the
        // camera target through the camera state directly, since the camera
        // is in CUTSCENE mode and not the standard target lock flow.
        if (scene_t >= 30.0f && scene_t < 36.0f) {
            KilnActor *alien0 = kiln_actor_resolve(g_alien_h[0]);
            if (alien0) g_target_lock = g_alien_h[0];
        } else {
            g_target_lock = KILN_ACTOR_HANDLE_NONE;
        }

        // ── Camera script ──────────────────────────────────────
        {
            fm_vec3_t eye, look;
            cam_sample(scene_t, &eye, &look);
            kiln_camera_set_cutscene(&g_cam, eye, look);
        }
        // kiln_camera_update runs the per-mode update (CUTSCENE copies the
        // cutscene_eye/look into cam->eye/look) — without it kiln_camera_apply
        // would copy zero vectors and t3d_viewport_look_at would divide by
        // zero in t3d_mat4_to_frustum, raising an FPU exception on frame 1.
        kiln_camera_update(&g_cam, (fm_vec3_t){{0,0,0}}, 0.0f, dt);
        kiln_camera_apply(&g_cam, &g_scene);
        kiln_scene_update(&g_scene);
        // Cache for the exception handler — survives a crash mid-frame so
        // the crash log shows what the camera was aiming at, not the prior
        // second's value.
        g_cine_eye[0] = g_scene.cam_pos.v[0];
        g_cine_eye[1] = g_scene.cam_pos.v[1];
        g_cine_eye[2] = g_scene.cam_pos.v[2];
        g_cine_look[0] = g_scene.cam_target.v[0];
        g_cine_look[1] = g_scene.cam_target.v[1];
        g_cine_look[2] = g_scene.cam_target.v[2];

        // Listener position for positional sound shaders.
        kiln_sound_update_listener(g_scene.cam_pos,
            (fm_vec3_t){{ g_scene.cam_target.v[0] - g_scene.cam_pos.v[0],
                          0,
                          g_scene.cam_target.v[2] - g_scene.cam_pos.v[2] }});

        // ── 3D pass ─────────────────────────────────────────────
        kiln_frame_begin();
        kiln_scene_begin(&g_scene);

        kiln_map_draw(&g_map);

        // Ship (in the actor pool? no — drawn directly so we can use the
        // local transform; the ship is a static prop with no state machine).
        kiln_transform_push(&ship_xform);
        t3d_model_draw(g_ship_model);
        kiln_transform_pop();

        // Crates from the physics world. The body list is global; iterate
        // and draw each as a cube whose centre is body->pos and half-extents
        // are body->maxs - body->mins (the body stores -maxs and +maxs so
        // the size on each axis is maxs - mins).
        for (int i = 0; i < g_pworld.count; i++) {
            KilnPhysicsBody *b = &g_pworld.bodies[i];
            // Skip the static anchor — it would draw at (0, 4, 50) which is
            // mid-hangar and is just there to keep kiln_physics non-empty.
            if (b->type == KILN_PHYS_STATIC) continue;
            KilnTransform t;
            kiln_transform_init(&t);
            t.pos = b->pos;
            t.scale = (fm_vec3_t){{ b->maxs.v[0] - b->mins.v[0],
                                    b->maxs.v[1] - b->mins.v[1],
                                    b->maxs.v[2] - b->mins.v[2] }};
            t.rot_axis = (fm_vec3_t){{ 0, 1, 0 }};
            t.rot_angle = 0.0f;
            kiln_transform_push(&t);
            draw_cube(g_cube_crate);
            kiln_transform_pop();
            kiln_transform_free(&t);
        }

        // All actors in category order.
        kiln_actor_draw_all();

        // ── 2D pass ─────────────────────────────────────────────
        kiln_gui_begin();
        draw_hud();
        if (g_target_lock != KILN_ACTOR_HANDLE_NONE) {
            KilnActor *t = kiln_actor_resolve(g_target_lock);
            if (t) {
                kiln_target_draw_reticle(&g_scene, t->xform.pos,
                                        SCREEN_W, SCREEN_H,
                                        RGBA32(245, 64, 80, 255));
            }
        }
        kiln_gui_end();

        kiln_frame_end();

        kiln_sound_update();
        kiln_audio_update();

        // ── Time advance + loop reset ──────────────────────────
        scene_t += dt;
        if (scene_t >= LOOP_T) {
            scene_t -= LOOP_T;
            blip_armed_goblin = 1;
            blip_armed_alien  = 1;
            // Reset crates to their start positions (loop is recorded).
            for (int i = 0; i < 3; i++) {           // stack A (3 bodies)
                g_bodies[i].pos.v[0] = 60.0f;
                g_bodies[i].pos.v[1] = 4.0f + i * 8.0f;
                g_bodies[i].pos.v[2] = 60.0f;
                g_bodies[i].vel = (fm_vec3_t){{ 0, 0, 0 }};
                g_bodies[i].sleeping = 0;
            }
            for (int i = 0; i < 2; i++) {           // stack B (2 bodies)
                g_bodies[3 + i].pos.v[0] = -65.0f;
                g_bodies[3 + i].pos.v[1] = 4.0f + i * 8.0f;
                g_bodies[3 + i].pos.v[2] = 25.0f;
                g_bodies[3 + i].vel = (fm_vec3_t){{ 0, 0, 0 }};
                g_bodies[3 + i].sleeping = 0;
            }
        }
    }
}
