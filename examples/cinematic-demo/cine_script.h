// SPDX-License-Identifier: MIT
//
// cine_script.h — where everyone in the hangar is, as a function of time.
//
// Every position in the cinematic is a pure function of `t` (seconds into the
// 60 s loop) rather than state integrated frame by frame. Three consumers need
// that: the ROM (which draws them), a jump ROM (which starts at t = 30 and must
// find the cast exactly where the base ROM has them at 30), and the camera,
// whose keys are authored against where the subject IS at each key's time.
// Integrating instead would make all three agree only by accident.
//
// Free of libdragon and Tiny3D beyond <t3d/t3dmath.h>, so the same header
// compiles natively.
//
// ── Scale: the one number the cast shares ────────────────────────────────
// gltf_to_t3d bakes ×64 into every model's vertices (nix/blender.nix's
// --base-scale=64), so a model is 64 units to the metre as loaded. The hangar
// (assets/hangar.map) is 200 × 200 units with a 56-unit ceiling: at ×64 the
// Interceptor alone is 345 units long and the goblin 140 tall — larger than
// the room, with every camera inside one of them. CINE_SCALE draws the whole
// cast at 6.4 units to the metre instead. One factor for everyone, so the
// models keep their authored proportions to each other.
//
// ── The bounds below are MEASURED, not guessed ──────────────────────────
// POSITION accessor min/max from each model's exported glTF
// ($out/share/gltf/<name>.gltf), in metres, engine axes (Y up). Every
// placement derives from them: a model's origin is not its feet unless the
// numbers say so (the Interceptor's origin is 0.36 m above its belly, the
// alien's 0.10 m below its feet), and CLAUDE.md's hard-won fact about a
// character floating a body-height off the floor is what happens otherwise.

#ifndef CINE_SCRIPT_H
#define CINE_SCRIPT_H

#include <t3d/t3dmath.h>

#define CINE_LOOP_T        60.0f
#define CINE_SCALE         0.1f                 /* draw scale for every model */
#define CINE_UNITS_PER_M   (64.0f * CINE_SCALE) /* 6.4 world units / metre    */
#define CINE_PI            3.14159265f

/* ── The room (assets/hangar.map) ──────────────────────────────────────── */
#define CINE_FLOOR_Y       0.4f
#define CINE_PAD_TOP       4.4f
#define CINE_PAD_HALF      30.0f
#define CINE_WALL_IN       96.0f   /* inner face of every hangar wall        */
#define CINE_CEIL_Y        56.0f
#define CINE_DOOR_HINGE_X  -80.0f  /* doorway spans x -80..-50, y 0..24     */
#define CINE_DOOR_W        30.0f
#define CINE_DOOR_H        24.0f
#define CINE_DOOR_Z        -98.0f  /* centre of the back wall's thickness     */

/* ── Model bounds, metres (see the file comment) ───────────────────────── */
typedef struct { float mn[3], mx[3]; } CineBounds;

static const CineBounds CINE_B_SHIP   = {{ -2.550f, -0.364f, -2.850f }, { 2.550f, 1.280f, 2.550f }};
static const CineBounds CINE_B_GOBLIN = {{ -0.844f,  0.016f, -0.688f }, { 0.844f, 2.188f, 0.359f }};
static const CineBounds CINE_B_DROID  = {{ -0.520f,  0.000f, -0.230f }, { 0.520f, 1.130f, 0.360f }};
static const CineBounds CINE_B_ALIEN  = {{ -0.390f,  0.100f, -0.280f }, { 0.390f, 2.810f, 0.280f }};

/* Which way each model's face points, as authored. The goblin and the ship
 * put their noses along Blender +Y (engine -Z); the droid and the alien put
 * their faces on Blender -Y (engine +Z). cine_yaw_to takes it into account. */
#define CINE_FWD_NEG_Z     0.0f
#define CINE_FWD_POS_Z     CINE_PI

/* Origin height that stands a model's lowest vertex on `surface_y`. */
static inline float cine_stand_y(const CineBounds *b, float surface_y)
{
    return surface_y - b->mn[1] * CINE_UNITS_PER_M;
}

/* ── Small helpers ─────────────────────────────────────────────────────── */
static inline float cine_clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
static inline float cine_smooth(float a, float b, float t)
{
    float u = cine_clamp01((t - a) / (b - a));
    return u * u * (3.0f - 2.0f * u);
}

/* Yaw for kiln_transform (rotation about +Y) that turns a model whose face is
 * at `fwd` (CINE_FWD_*) toward direction (dx, dz).
 *
 * libdragon's fm_mat4_from_axis_angle about +Y maps -Z to (sin a, 0, -cos a)
 * and +Z to (-sin a, 0, cos a) — measured natively against the host build of
 * libdragon's own fast math. This used to assume (-sin a, 0, -cos a), the
 * right-hand rule, which is right only along the Z axis: the whole cast faced
 * mirrored whenever it moved sideways, the captain walking his circle with his
 * nose pointing out of it. */
static inline float cine_yaw_to(float dx, float dz, float fwd)
{
    return fm_atan2f(dx, -dz) + fwd;
}

/* The direction a transform yaw from cine_yaw_to actually faces, as an
 * fm_atan2f(x, z) angle — what a head turn is measured against. */
static inline float cine_facing(float yaw, float fwd)
{
    return fwd == CINE_FWD_NEG_Z ? CINE_PI - yaw : -yaw;
}

static inline float cine_wrap(float a)
{
    while (a >  CINE_PI) a -= 2.0f * CINE_PI;
    while (a < -CINE_PI) a += 2.0f * CINE_PI;
    return a;
}

/* Shortest-way blend between two angles. */
static inline float cine_angle_lerp(float a, float b, float k)
{
    float d = b - a;
    while (d >  CINE_PI) d -= 2.0f * CINE_PI;
    while (d < -CINE_PI) d += 2.0f * CINE_PI;
    return a + d * k;
}

/* Catmull-Rom through four points; the curve an alien walks. */
static inline float cine_cr(float p0, float p1, float p2, float p3, float f)
{
    const float f2 = f * f, f3 = f2 * f;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * f
                 + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * f2
                 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * f3);
}

/* One actor's pose at time t. `walk` is 0 standing .. 1 walking, and drives
 * the kiln_skel blend. */
typedef struct {
    fm_vec3_t pos;
    float     yaw;
    float     walk;
} CinePose;

/* ── The goblin captain ────────────────────────────────────────────────── */
// Walks the perimeter of the pad (r = 50) and stops for twelve seconds facing
// the doorway while the aliens come out. The stop is eased: `hold` is the
// integral of a smoothstep-shaped stop factor, so his walked time w(t) slows
// to zero over two seconds rather than stepping, and the full lap takes the
// 46 seconds of walking in the loop — so t = 60 puts him back where t = 0 did.
#define CINE_GOB_R          50.0f
#define CINE_GOB_STOP_IN    26.0f   /* starts slowing              */
#define CINE_GOB_STOP_AT    28.0f   /* stood still                 */
#define CINE_GOB_GO_AT      40.0f   /* starts walking again        */
#define CINE_GOB_GO_FULL    42.0f   /* back to full pace           */
#define CINE_GOB_WALKED     46.0f   /* seconds of walking per loop */
#define CINE_GOB_THETA0     0.43f   /* chosen so he stops facing the door */
#define CINE_GOB_OMEGA      (2.0f * CINE_PI / CINE_GOB_WALKED)

/* Seconds of walking done by time t (0..46). */
static inline float cine_goblin_walked(float t)
{
    float hold;
    if (t < CINE_GOB_STOP_IN) hold = 0.0f;
    else if (t < CINE_GOB_STOP_AT) {
        float u = (t - CINE_GOB_STOP_IN) / (CINE_GOB_STOP_AT - CINE_GOB_STOP_IN);
        hold = 2.0f * (u * u * u - 0.5f * u * u * u * u);
    } else if (t < CINE_GOB_GO_AT) hold = 1.0f + (t - CINE_GOB_STOP_AT);
    else if (t < CINE_GOB_GO_FULL) {
        float u = (t - CINE_GOB_GO_AT) / (CINE_GOB_GO_FULL - CINE_GOB_GO_AT);
        hold = 13.0f + 2.0f * (u - (u * u * u - 0.5f * u * u * u * u));
    } else hold = 14.0f;
    return t - hold;
}

/* How fast he is covering ground at t, units/s: his arc speed times the rate
 * walked time is passing. The Walk clip plays at this over its measured ground
 * speed, or his feet skate. */
static inline float cine_goblin_speed(float t)
{
    const float h = 0.02f;
    const float lo = t - h < 0.0f ? 0.0f : t - h;
    const float dw = (cine_goblin_walked(t + h) - cine_goblin_walked(lo)) / (t + h - lo);
    return CINE_GOB_R * CINE_GOB_OMEGA * dw;
}

static inline CinePose cine_goblin(float t)
{
    CinePose p;
    const float w = cine_goblin_walked(t);
    const float th = CINE_GOB_THETA0 + CINE_GOB_OMEGA * w;
    const float c = fm_cosf(th), s = fm_sinf(th);
    p.pos = (fm_vec3_t){{ c * CINE_GOB_R, cine_stand_y(&CINE_B_GOBLIN, CINE_FLOOR_Y), s * CINE_GOB_R }};
    p.walk = 1.0f - cine_smooth(CINE_GOB_STOP_IN, CINE_GOB_STOP_AT, t)
                  * (1.0f - cine_smooth(CINE_GOB_GO_AT, CINE_GOB_GO_FULL, t));
    /* Walking: along the tangent (θ increasing). Stopped: toward the doorway,
     * where the aliens come from. */
    const float walk_yaw = cine_yaw_to(-s, c, CINE_FWD_NEG_Z);
    const float look_yaw = cine_yaw_to(-65.0f - p.pos.v[0], -80.0f - p.pos.v[2], CINE_FWD_NEG_Z);
    p.yaw = cine_angle_lerp(walk_yaw, look_yaw, 1.0f - p.walk);
    return p;
}

/* ── Service droids ────────────────────────────────────────────────────── */
// Four stations a quarter-turn apart around the ship, on the pad. Each 7.5 s
// cycle is five seconds parked facing the hull (waving, which is what the
// Wave clip is) and 2.5 s rolling to the next station facing the way they go.
// Eight quarter-turns in 60 s is two laps, so the loop closes.
#define CINE_DROID_CYCLE    7.5f
#define CINE_DROID_PARK     5.0f

static inline CinePose cine_droid(float radius, float phase, float t)
{
    CinePose p;
    const float cyc = t / CINE_DROID_CYCLE;
    const int   k   = (int)cyc;
    const float tau = t - (float)k * CINE_DROID_CYCLE;
    const float move = cine_smooth(CINE_DROID_PARK, CINE_DROID_CYCLE, tau);
    const float th = phase + 0.5f * CINE_PI * ((float)k + move);
    const float c = fm_cosf(th), s = fm_sinf(th);
    p.pos = (fm_vec3_t){{ c * radius, cine_stand_y(&CINE_B_DROID, CINE_PAD_TOP), s * radius }};
    /* 1 while rolling, 0 while parked, with a short ease either side. */
    p.walk = cine_smooth(CINE_DROID_PARK - 0.3f, CINE_DROID_PARK + 0.3f, tau)
           * (1.0f - cine_smooth(CINE_DROID_CYCLE - 0.4f, CINE_DROID_CYCLE, tau));
    const float in_yaw   = cine_yaw_to(-c, -s, CINE_FWD_POS_Z);
    const float move_yaw = cine_yaw_to(-s, c, CINE_FWD_POS_Z);
    p.yaw = cine_angle_lerp(in_yaw, move_yaw, p.walk);
    return p;
}

/* ── Aliens ────────────────────────────────────────────────────────────── */
// Out of the corridor through the door (which opens at 22.5 s), along a curve
// to a stand-off with the goblin, twelve-odd seconds facing him, and back the
// way they came before the door closes at 55 s. The path is a Catmull-Rom
// through four waypoints, walked with an eased parameter so they set off and
// arrive rather than sliding at constant speed.
typedef struct {
    float wx[4], wz[4];     /* corridor start, doorway, mid, stand-off     */
    float out0, out1;       /* walk out: start and arrival times           */
    float back0, back1;     /* walk back                                   */
} CineAlienPath;

static const CineAlienPath CINE_ALIEN_PATHS[2] = {
    /* lead: the reticle locks onto this one */
    { { -66.0f, -65.0f, -58.0f, -46.0f }, { -126.0f, -104.0f, -84.0f, -66.0f },
      23.0f, 31.0f, 44.0f, 52.0f },
    /* late: comes out behind and wide, to the left of the lead */
    { { -62.0f, -65.0f, -72.0f, -76.0f }, { -134.0f, -104.0f, -86.0f, -60.0f },
      24.5f, 33.0f, 45.0f, 53.5f },
};

/* Point on the path at u in 0..1 (the four waypoints, ends clamped). */
static inline void cine_alien_path_at(const CineAlienPath *a, float u, float *x, float *z)
{
    u = cine_clamp01(u) * 3.0f;
    int i = (int)u;
    if (i > 2) i = 2;
    const float f = u - (float)i;
    const int i0 = i > 0 ? i - 1 : 0, i1 = i, i2 = i + 1, i3 = i + 2 < 4 ? i + 2 : 3;
    *x = cine_cr(a->wx[i0], a->wx[i1], a->wx[i2], a->wx[i3], f);
    *z = cine_cr(a->wz[i0], a->wz[i1], a->wz[i2], a->wz[i3], f);
}

/* Path parameter at time t: 0 in the corridor, 1 at the stand-off. */
static inline float cine_alien_u(const CineAlienPath *a, float t)
{
    if (t < a->back0) return cine_smooth(a->out0, a->out1, t);
    return 1.0f - cine_smooth(a->back0, a->back1, t);
}

static inline CinePose cine_alien(int which, float t, fm_vec3_t goblin)
{
    const CineAlienPath *a = &CINE_ALIEN_PATHS[which & 1];
    CinePose p;
    float x, z, x2, z2;
    const float u = cine_alien_u(a, t);
    cine_alien_path_at(a, u, &x, &z);
    p.pos = (fm_vec3_t){{ x, cine_stand_y(&CINE_B_ALIEN, CINE_FLOOR_Y), z }};

    /* Walking while the eased parameter is moving; the ramps are the
     * smoothstep's own shoulders, so the legs stop when the body does. */
    const float in_out  = cine_smooth(a->out0, a->out0 + 1.0f, t) * (1.0f - cine_smooth(a->out1 - 1.0f, a->out1, t));
    const float in_back = cine_smooth(a->back0, a->back0 + 1.0f, t) * (1.0f - cine_smooth(a->back1 - 1.0f, a->back1, t));
    p.walk = in_out + in_back;

    /* Facing: along the path the way it is travelling (a finite difference
     * along the curve itself), and toward the goblin while stood off. */
    cine_alien_path_at(a, cine_clamp01(u - 0.01f), &x, &z);
    cine_alien_path_at(a, cine_clamp01(u + 0.01f), &x2, &z2);
    float dx = x2 - x, dz = z2 - z;
    if (t >= a->back0) { dx = -dx; dz = -dz; }
    const float path_yaw = cine_yaw_to(dx, dz, CINE_FWD_POS_Z);
    const float face_yaw = cine_yaw_to(goblin.v[0] - p.pos.v[0], goblin.v[2] - p.pos.v[2], CINE_FWD_POS_Z);
    /* Turn to face him over the last second of the walk out; turn away over
     * the first second of the walk back. */
    const float facing = cine_smooth(a->out1 - 1.0f, a->out1, t) * (1.0f - cine_smooth(a->back0, a->back0 + 1.0f, t));
    p.yaw = cine_angle_lerp(path_yaw, face_yaw, facing);
    return p;
}

/* ── Acting ────────────────────────────────────────────────────────────── */
// One-shot clips played over the walk on kiln_skel's overlay slot, as windows
// of t, so a jump ROM that lands inside one is mid-gesture. `upper` masks the
// clip to the torso's subtree: a wave does not stop the legs.
typedef struct {
    const char *clip;
    float start, len;
    unsigned char upper;
} CineBeat;

static const CineBeat CINE_GOBLIN_BEATS[] = {
    { "Wave",  16.0f, 50.0f / 24.0f, 1 },   /* to the droids, still walking  */
    { "Taunt", 33.0f, 60.0f / 24.0f, 0 },   /* at the aliens, stood off       */
    { "Wave",  49.0f, 50.0f / 24.0f, 1 },   /* see you round                  */
};
#define CINE_GOBLIN_BEAT_COUNT ((int)(sizeof(CINE_GOBLIN_BEATS) / sizeof(CINE_GOBLIN_BEATS[0])))

static inline int cine_beat_at(const CineBeat *b, int n, float t)
{
    for (int i = 0; i < n; i++)
        if (t >= b[i].start && t < b[i].start + b[i].len) return i;
    return -1;
}

/* Where the captain's head looks while he stands off, and how much. */
#define CINE_GOB_LOOK_ON   24.0f
#define CINE_GOB_LOOK_OFF  43.5f

/* ── Staging ───────────────────────────────────────────────────────────── */
/* The alarm light: red from the door opening until the stand-off settles,
 * pulsing at 1.6 Hz between a third and full. Never to zero, or a frame caught
 * in the trough (t = 30, say, which is 48 whole cycles) shows no alarm. 0..1. */
static inline float cine_alarm(float t)
{
    const float on = cine_smooth(22.4f, 22.9f, t) * (1.0f - cine_smooth(30.5f, 32.0f, t));
    const float s = 0.5f + 0.5f * fm_sinf(t * 2.0f * CINE_PI * 1.6f);
    return on * (0.35f + 0.65f * s);
}

/* Timed subtitles, typed out in the lower letterbox bar. */
typedef struct {
    float start, end;
    const char *who, *line;
} CineLine;

static const CineLine CINE_LINES[] = {
    { 33.2f, 36.4f, "CAPTAIN", "Easy. Nobody reach for anything." },
    { 36.8f, 40.2f, "ALIEN",   "This dock is ours, small one." },
    { 41.0f, 44.0f, "CAPTAIN", "Then we both leave. Slowly." },
    { 50.0f, 53.5f, "CAPTAIN", "Log it. Nothing happened here." },
};
#define CINE_LINE_COUNT ((int)(sizeof(CINE_LINES) / sizeof(CINE_LINES[0])))

/* ── Door ──────────────────────────────────────────────────────────────── */
#define CINE_DOOR_OPEN_T    22.0f   /* event posted; lands 500 ms later */
#define CINE_DOOR_CLOSE_T   54.5f

/* ── Crates ────────────────────────────────────────────────────────────── */
// Stack A stands just outside the goblin's circle where he passes it at
// CINE_BUMP_A_T; stack B beside the late alien's walk out.
#define CINE_CRATE_HALF     4.0f
#define CINE_STACK_A_X      -10.4f  /* r = 60 at 100°                     */
#define CINE_STACK_A_Z      59.1f
#define CINE_STACK_B_X      -86.0f
#define CINE_STACK_B_Z      -76.0f
#define CINE_BUMP_A_T       9.6f
#define CINE_BUMP_B_T       29.5f

#endif // CINE_SCRIPT_H
