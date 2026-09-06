/* SPDX-License-Identifier: MIT
 *
 * forge.h — the editor's shared state.
 *
 * Forge is a voxel builder whose save button emits the content formats this
 * engine already consumes: Quake `.map` brushes for kiln_map/kiln_clip, and (as
 * the modes land) a PMCamKey table, entity epairs, a light rig and a CI4
 * texture atlas. It runs ON the console because that is where the judgements
 * are — fill rate, whether a corridor reads as a corridor, whether a palette
 * still separates once the veil discards hue. Every other authoring tool in
 * this repo runs on the host and is two hops from the thing it describes.
 *
 * One struct, statically allocated, passed by pointer. No globals scattered
 * across modes and no allocator: the world is ~100 KB and the vertex arena is
 * sized once at boot, because a heap failure partway through a remesh is not
 * recoverable on this console.
 */
#ifndef FORGE_H
#define FORGE_H

#include <libdragon.h>
#include <kiln/kiln_engine.h>
#include <kiln/kiln_gui.h>
#include <kiln/kiln_input.h>
#include <kiln/kiln_clip.h>
#include <kiln/kiln_fpscam.h>
#include <kiln/kiln_debugdraw.h>
#include <kiln/kiln_store.h>
#include <kiln/kiln_voxel.h>
#include <kiln/kiln_voxmesh.h>
#include <kiln/kiln_camkey.h>
#include <kiln/kiln_camlint.h>

#define FORGE_SCREEN_W 320
#define FORGE_SCREEN_H 240

/* Vertex arena. 6144 T3DVertPacked entries = 12288 vertices = 3072 quads
 * across every meshed chunk, at 192 KB uncached.
 *
 * Sized against fill rate rather than against RDRAM: budgets that held 60 fps
 * in this engine are ~830 triangles for island terrain, and 3072 quads is 6144
 * triangles. So this arena is deliberately larger than what will draw smoothly
 * — the editor reports its own frame rate and is allowed to run at 30, and it
 * is much worse for a wall to be missing than for the frame to be slow. The
 * gauge on the HUD is what makes the trade visible instead of mysterious. */
#define FORGE_ARENA_ENTRIES 6144

/* Per-chunk quad scratch. A pathological checkerboard chunk would need 12288;
 * a room's shell needs a few hundred. 2048 is the honest middle, and going over
 * it is REPORTED rather than silently truncated. */
#define FORGE_QUAD_SCRATCH 2048

/* Collision boxes for WALK mode. Matches kiln_room.h's own
 * KILN_ROOM_MAX_CLIP_BRUSHES, and note the deliberate decision in forge_walk.c
 * not to enable kiln_clip's broadphase. */
#define FORGE_MAX_BOXES 512

/* Mode order is the authoring order: block it out, walk it, texture it, dress
 * it, light it, shoot it. L+R cycles, so the sequence you cycle through is the
 * sequence you work in. */
typedef enum {
    FORGE_MODE_GEO = 0,   /* place and break blocks                     */
    FORGE_MODE_WALK,      /* drop into the level with real collision    */
    FORGE_MODE_PAINT,     /* the CI4 atlas: draw tiles, assign to types */
    FORGE_MODE_ENT,       /* classnames, origins, angles, epairs        */
    FORGE_MODE_LIGHT,     /* key/fill/ambient/fog on KilnScene           */
    FORGE_MODE_CAM,       /* keyframe a shot and validate it            */
    FORGE_MODE_COUNT,
} ForgeMode;

/* ── ENT ───────────────────────────────────────────────────────────────
 * 64 matches kiln_map.c's MAX_SPAWNS exactly. Matching the consumer's cap rather
 * than picking a round number means the editor cannot author a level the parser
 * will silently truncate — kiln_map.c drops spawns past its cap without a word. */
#define FORGE_MAX_ENTS    64
/* Both counts come from the generated vocabulary, so adding a classname to
 * tools/schema/level_vocab.json is one edit rather than one edit plus
 * remembering this file. */
#include "forge_vocab.gen.h"
#define FORGE_CLASSNAMES   FORGE_VOCAB_CLASSNAME_COUNT
#define FORGE_EPAIRS       FORGE_VOCAB_EPAIR_COUNT

typedef struct {
    fm_vec3_t pos;
    int16_t   angle;                  /* degrees, as the .map "angle" epair */
    uint8_t   classname;              /* index into forge_ent_classname()   */
    uint8_t   _pad;
    int32_t   epair[FORGE_EPAIRS];    /* 0 means "not set" and is omitted   */
} ForgeEnt;

/* ── LIGHT ─────────────────────────────────────────────────────────────*/
typedef enum {
    FORGE_LF_KEY_DIR = 0, FORGE_LF_KEY_LEVEL,
    FORGE_LF_FILL_DIR,    FORGE_LF_FILL_LEVEL,
    FORGE_LF_AMBIENT,     FORGE_LF_FOG_NEAR, FORGE_LF_FOG_FAR,
    FORGE_LIGHT_FIELDS,
} ForgeLightField;

/* ── CAM ───────────────────────────────────────────────────────────────
 * 32 keys is generous against real tables built with this system: a
 * downstream game's longest shot used 17, including three keys inserted
 * purely to suppress overshoot. */
#define FORGE_MAX_KEYS 32

typedef struct {
    /* ── World ─────────────────────────────────────────────────────────*/
    KilnVoxelWorld   world;
    KilnVoxAtlas     atlas;
    KilnVoxMeshArena arena;
    KilnVoxMesh      meshes[KILN_VOXEL_MAX_CHUNKS];
    int             mesh_valid[KILN_VOXEL_MAX_CHUNKS];

    /* ── Camera ────────────────────────────────────────────────────────*/
    KilnScene  scene;
    fm_vec3_t fly_pos;
    float     fly_yaw, fly_pitch;
    KilnFpsCam walk_cam;

    /* ── PAINT ─────────────────────────────────────────────────────────*/
    int paint_tile, paint_x, paint_y;
    uint8_t paint_colour;
    int paint_veiled;

    /* ── ENT ───────────────────────────────────────────────────────────*/
    ForgeEnt ents[FORGE_MAX_ENTS];
    int      ent_count, ent_sel, ent_hover, ent_class, ent_field, ent_full;

    /* ── LIGHT ─────────────────────────────────────────────────────────*/
    int   light_field, fog_on, clear_idx;
    float key_yaw, key_pitch, fill_yaw, fill_pitch;
    float fog_near, fog_far;
    uint8_t key_level, fill_level, ambient;

    /* ── CAM ───────────────────────────────────────────────────────────*/
    KilnCamKey    keys[FORGE_MAX_KEYS];
    int          key_count, key_sel, key_full;
    float        cine_t, cine_duration;
    int          cine_loop, cine_playing, cine_frustum_hidden;
    uint32_t     cine_err;
    KilnCamReport cine_report;

    /* ── Editing ───────────────────────────────────────────────────────*/
    ForgeMode   mode;
    uint8_t     block;         /* the type being placed, 1..15          */
    KilnVoxelHit aim;           /* this frame's reticle result           */
    int         drag_active;   /* Z held: a fill volume is being dragged */
    int         drag[3];       /* its anchor block                       */

    /* ── Status, all of it displayed ───────────────────────────────────*/
    int        remesh_overflow;  /* a chunk's quads did not fit          */
    int        arena_overflow;   /* the vertex arena ran out             */
    int        box_overflow;     /* greedy boxes exceeded FORGE_MAX_BOXES */
    int        fell;            /* WALK caught a fall out of the level    */
    int        last_store;       /* KilnStoreStatus of the last save/load  */
    const char *last_action;     /* what that status belongs to           */
    uint32_t   boxes_used;
    uint32_t   quads_drawn;   /* merged surface quads across every chunk */
    float      fps;

    /* ── Scratch, here rather than on the stack ────────────────────────*/
    KilnVoxelQuad quads[FORGE_QUAD_SCRATCH];
    KilnBrush     boxes[FORGE_MAX_BOXES];
} Forge;

/* The level name every artifact is written under. One name, so a session's
 * .FRG, .MAP and .LOG cannot drift apart. */
#define FORGE_LEVEL_NAME "LEVEL"

/* forge_cam.c */
void forge_cam_init(Forge *f);
void forge_cam_update(Forge *f, const KilnInput *in, float dt);
void forge_cam_apply(Forge *f);
fm_vec3_t forge_cam_forward(const Forge *f);

/* forge_geo.c */
void forge_geo_update(Forge *f, const KilnInput *in);
void forge_geo_remesh(Forge *f);
void forge_geo_draw(Forge *f);
void forge_geo_draw_overlay(Forge *f);

/* forge_walk.c */
void forge_walk_enter(Forge *f);
fm_vec3_t forge_walk_spawn(const Forge *f);
void forge_walk_update(Forge *f, const KilnInput *in, float dt);

/* forge_paint.c */
void forge_paint_update(Forge *f, const KilnInput *in);
void forge_paint_draw(Forge *f);

/* forge_ent.c */
void forge_ent_update(Forge *f, const KilnInput *in);
void forge_ent_draw(Forge *f);
void forge_ent_draw3d(Forge *f);
int  forge_ent_emit(const Forge *f, char *out, int cap);
const char *forge_ent_classname(int i);
const char *forge_ent_epair_key(int i);

/* forge_light.c */
void forge_light_update(Forge *f, const KilnInput *in);
void forge_light_apply(Forge *f);
void forge_light_draw(Forge *f);
void forge_light_draw3d(Forge *f);
int  forge_light_emit(const Forge *f, char *out, int cap);

/* forge_cine.c */
void forge_cine_update(Forge *f, const KilnInput *in, float dt);
void forge_cine_validate(Forge *f);
int  forge_cine_override_camera(Forge *f);
void forge_cine_draw(Forge *f);
void forge_cine_draw3d(Forge *f);
int  forge_cine_emit(const Forge *f, char *out, int cap);

/* forge_hud.c */
void forge_hud_draw(Forge *f);

/* forge_io.c */
int  forge_io_save(Forge *f);
int  forge_io_load(Forge *f);
void forge_io_seed(Forge *f);

#endif /* FORGE_H */
