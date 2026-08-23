/* SPDX-License-Identifier: MIT
 *
 * forge_geo.c — placing and breaking blocks, and getting the result on screen.
 *
 * The mesh cache is the only thing here with any subtlety. A chunk's geometry is
 * rebuilt when kiln_voxel marks it dirty, and because the vertex arena is a bump
 * allocator with no free list, rebuilding ONE chunk means repacking ALL of them.
 * That sounds wasteful and is the right trade: an arena with per-chunk frees
 * fragments, and the alternative — a fixed per-chunk slice — would have to be
 * sized for the pathological chunk (12288 quads) rather than the typical one
 * (a few hundred), so it would cost 4x the RDRAM to hold the same level.
 * Repacking 24 chunks is a few thousand vertex writes; it happens on an edit,
 * not per frame.
 */
#include "forge.h"

/* One state setup for every chunk, not one per chunk. kiln_voxmesh_draw sets no
 * render state by design, so this is the only place the combiner and the atlas
 * are chosen — the same arrangement kiln_map_draw relies on. */
static void begin_voxel_state(Forge *f)
{
    /* TEXTURED | SHADED so the per-vertex direction shade multiplies the tile.
     * NO_LIGHT because the shade is already baked per face: letting the scene's
     * directional light multiply it again is the same double-darkening mistake
     * the lab's pre-lit vertex colours produced, where "the lab reads as unlit"
     * was in fact the lab being lit twice. */
    t3d_state_set_drawflags(T3D_FLAG_TEXTURED | T3D_FLAG_SHADED |
                            T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
    kiln_voxatlas_bind(&f->atlas, KILN_VOXATLAS_COLD);
}

void forge_geo_remesh(Forge *f)
{
    kiln_voxmesh_arena_reset(&f->arena);
    f->remesh_overflow = 0;
    f->arena_overflow = 0;
    f->quads_drawn = 0;

    for (int s = 0; s < KILN_VOXEL_MAX_CHUNKS; s++) {
        f->mesh_valid[s] = 0;
        memset(&f->meshes[s], 0, sizeof f->meshes[s]);
    }

    for (int s = kiln_voxel_slot_first(&f->world); s >= 0;
             s = kiln_voxel_slot_next(&f->world, s)) {
        int nq = kiln_voxel_quads(&f->world, s, f->quads, FORGE_QUAD_SCRATCH);
        if (nq < 0) {
            /* The chunk needed more quads than the scratch holds. Mesh what
             * fits rather than dropping the chunk: a partly-drawn chunk with a
             * red gauge beside it is diagnosable, a vanished one is not. */
            f->remesh_overflow = -nq;
            nq = kiln_voxel_quads(&f->world, s, f->quads, FORGE_QUAD_SCRATCH - 1);
            if (nq < 0) nq = FORGE_QUAD_SCRATCH - 1;
        }
        if (nq == 0) continue;

        if (kiln_voxmesh_build(&f->world, s, f->quads, (uint32_t)nq,
                              &f->arena, KILN_VOXATLAS_ACROSS,
                              &f->meshes[s]) != 0) {
            f->arena_overflow = 1;
            break;      /* the arena is a bump allocator: later chunks cannot fit either */
        }
        f->mesh_valid[s] = 1;
        f->quads_drawn += (uint32_t)nq;
        f->world.chunks[s].dirty = 0;
    }
}

static int any_dirty(const Forge *f)
{
    for (int s = kiln_voxel_slot_first(&f->world); s >= 0;
             s = kiln_voxel_slot_next(&f->world, s))
        if (f->world.chunks[s].dirty) return 1;
    return 0;
}

void forge_geo_update(Forge *f, const KilnInput *in)
{
    /* Aim first: every edit this frame uses the same reticle result, so the
     * block you saw highlighted is the block that changes. Recomputing the ray
     * per action would let a place and a break in the same frame disagree. */
    fm_vec3_t fwd = forge_cam_forward(f);
    const float B = (float)KILN_VOXEL_BLOCK_UNITS;
    kiln_voxel_raycast(&f->world, &f->fly_pos, &fwd, 64.0f * B, &f->aim);

    int edited = 0;

    /* Z + A drags a fill volume: press Z to anchor at the current aim, release
     * to fill. Z alone does nothing, so an accidental Z is harmless. */
    if ((in->edges & KILN_BTN_Z) && f->aim.hit) {
        f->drag_active = 1;
        f->drag[0] = f->aim.px; f->drag[1] = f->aim.py; f->drag[2] = f->aim.pz;
    }
    if (f->drag_active && (in->released & KILN_BTN_Z)) {
        f->drag_active = 0;
        if (f->aim.hit) {
            int n = kiln_voxel_fill(&f->world, f->drag[0], f->drag[1], f->drag[2],
                                   f->aim.px, f->aim.py, f->aim.pz, f->block);
            if (n != 0) edited = 1;
        }
    }

    if (!f->drag_active) {
        /* Edges, not held state: one press is one block. A held A that placed a
         * block per frame would fill a corridor in half a second. */
        if ((in->edges & KILN_BTN_A) && f->aim.hit) {
            if (kiln_voxel_set(&f->world, f->aim.px, f->aim.py, f->aim.pz,
                              f->block) == 0) edited = 1;
        }
        if ((in->edges & KILN_BTN_B) && f->aim.hit) {
            if (kiln_voxel_set(&f->world, f->aim.x, f->aim.y, f->aim.z,
                              KILN_VOXEL_AIR) == 0) edited = 1;
        }
    }

    /* Block palette on the shoulder-free D-pad axis. B-picks-the-aimed-type is
     * the Minecraft middle-click and is worth having: it is how you match a
     * material you placed twenty blocks ago without counting through 15 types. */
    if (in->edges & KILN_BTN_DR)
        f->block = (uint8_t)(f->block % KILN_VOXEL_TYPE_MAX + 1);
    if (in->edges & KILN_BTN_DL)
        f->block = (uint8_t)(f->block <= 1 ? KILN_VOXEL_TYPE_MAX : f->block - 1);
    if ((in->edges & KILN_BTN_CU) == 0 && (in->edges & KILN_BTN_L) && f->aim.hit)
        f->block = f->aim.block;

    if (edited || any_dirty(f)) forge_geo_remesh(f);
}

void forge_geo_draw(Forge *f)
{
    begin_voxel_state(f);
    for (int s = 0; s < KILN_VOXEL_MAX_CHUNKS; s++)
        if (f->mesh_valid[s]) kiln_voxmesh_draw(&f->meshes[s]);
}

/* The reticle and the drag volume, drawn in the 2D pass through
 * kiln_scene_project (kiln_debugdraw's whole approach), so they cannot perturb
 * the frame they describe and stay visible through geometry — which for an
 * edit cursor is the point, not a compromise. */
void forge_geo_draw_overlay(Forge *f)
{
    const float B = (float)KILN_VOXEL_BLOCK_UNITS;
    kiln_dd_begin(&f->scene, FORGE_SCREEN_W, FORGE_SCREEN_H);

    if (f->aim.hit) {
        fm_vec3_t mn = {{ f->world.offset.v[0] + (float)f->aim.x * B,
                          f->world.offset.v[1] + (float)f->aim.y * B,
                          f->world.offset.v[2] + (float)f->aim.z * B }};
        fm_vec3_t mx = {{ mn.v[0] + B, mn.v[1] + B, mn.v[2] + B }};
        kiln_dd_aabb(mn, mx, RGBA32(255, 220, 60, 255));

        /* The cell a new block would occupy, in a different colour. Showing both
         * is what makes "am I placing on this face or in that gap" answerable
         * without trying it — the classic voxel-editor misplacement.
         *
         * RED when that cell is off the grid, which happens constantly and
         * legitimately: aim at the outside of a boundary wall and the place-here
         * cell is outside the world. kiln_voxel_set no-ops there, correctly, so
         * without this the editor draws an inviting cyan box at a position where
         * pressing A does nothing — and a control that silently does nothing is
         * read as a broken control. */
        if (!(f->aim.px == f->aim.x && f->aim.py == f->aim.y && f->aim.pz == f->aim.z)) {
            fm_vec3_t pn = {{ f->world.offset.v[0] + (float)f->aim.px * B,
                              f->world.offset.v[1] + (float)f->aim.py * B,
                              f->world.offset.v[2] + (float)f->aim.pz * B }};
            fm_vec3_t px = {{ pn.v[0] + B, pn.v[1] + B, pn.v[2] + B }};
            int can = kiln_voxel_in_bounds(f->aim.px, f->aim.py, f->aim.pz);
            kiln_dd_aabb(pn, px, can ? RGBA32(80, 200, 255, 200)
                                    : RGBA32(255, 80, 70, 200));
        }
    }

    if (f->drag_active && f->aim.hit) {
        int lo[3], hi[3];
        int a[3] = { f->drag[0], f->drag[1], f->drag[2] };
        int b[3] = { f->aim.px, f->aim.py, f->aim.pz };
        for (int i = 0; i < 3; i++) {
            lo[i] = a[i] < b[i] ? a[i] : b[i];
            hi[i] = a[i] > b[i] ? a[i] : b[i];
        }
        fm_vec3_t mn = {{ f->world.offset.v[0] + (float)lo[0] * B,
                          f->world.offset.v[1] + (float)lo[1] * B,
                          f->world.offset.v[2] + (float)lo[2] * B }};
        fm_vec3_t mx = {{ f->world.offset.v[0] + (float)(hi[0] + 1) * B,
                          f->world.offset.v[1] + (float)(hi[1] + 1) * B,
                          f->world.offset.v[2] + (float)(hi[2] + 1) * B }};
        kiln_dd_aabb(mn, mx, RGBA32(120, 255, 140, 255));
    }

    kiln_dd_end();
}
