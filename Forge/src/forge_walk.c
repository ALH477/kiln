/* SPDX-License-Identifier: MIT
 *
 * forge_walk.c — stand in the level with the collision the game will use.
 *
 * The point of the whole tool is that judgements about a space are made in the
 * space. A free-fly camera cannot tell you whether a doorway is wide enough,
 * whether a step is climbable, or whether a corridor feels like a corridor — so
 * WALK mode installs the greedy-meshed boxes as the clip world and hands the pad
 * to the real kiln_fpscam. Not an approximation of the game's movement: the same
 * module, the same slide, the same ground probe.
 *
 * ── The broadphase stays OFF, deliberately ─────────────────────────────
 *
 * kiln_clip's broadphase grid is 16x16 in XZ with Y ignored, and its placement
 * pass asserts `acc <= CLIP_GRID_POOL` (512) where a brush is listed in every
 * cell it overlaps. A voxel-derived brush set is exactly the shape that trips
 * that: a floor slab spanning the room lands in a great many cells. The assert
 * is a hard crash, and a tool that dies when you press the walk button is worse
 * than one that traces a flat list. 512 boxes x a flat walk x 4 slide iterations
 * is a few thousand AABB tests a frame, which this CPU does not notice.
 */
#include "forge.h"

void forge_walk_enter(Forge *f)
{
    /* Boxes first: without a clip world kiln_fpscam's ground probe finds nothing
     * and gravity takes the player through the floor forever. That exact
     * composition — a working camera over an empty clip world — is what made
     * PLAY look like a black screen with a working HUD for its entire life, and
     * it is why the HUD prints the box count in red at zero. */
    int nb = kiln_voxel_boxes(&f->world, f->boxes, FORGE_MAX_BOXES, NULL);
    if (nb < 0) {
        /* Over the cap: install what fits and SAY so. The alternative — refuse
         * to enter WALK — hides the level from the one check that matters most,
         * and the alternative to saying so is a player falling through a wall
         * that is visibly there. */
        f->box_overflow = -nb;
        nb = kiln_voxel_boxes(&f->world, f->boxes, FORGE_MAX_BOXES - 1, NULL);
        if (nb < 0) nb = 0;
    } else {
        f->box_overflow = 0;
    }
    f->boxes_used = (uint32_t)nb;
    kiln_clip_set_world(f->boxes, (uint16_t)nb);
    kiln_clip_set_broadphase(0);   /* see the file comment */

    kiln_fpscam_init(&f->walk_cam);

    kiln_fpscam_snap(&f->walk_cam, forge_walk_spawn(f), f->fly_yaw, 0.0f);
}

/* Where to stand. Three fallbacks, in order of how much they know:
 *
 *   1. the cell the reticle is aiming at — the user has literally pointed at it;
 *   2. above the CENTRE of the level's own bounds — the level knows where it is;
 *   3. the fly camera, as a last resort.
 *
 * The first version had only (1) and (3), and (3) is wrong often enough to
 * matter: a boot straight into WALK has never run the reticle, and the fly
 * camera starts deliberately OUTSIDE the level looking in. So the player spawned
 * over empty space with nothing under them, and `.#forge-walk`'s first capture
 * read `eye -192 -16814 -192` — sixteen thousand units below the floor, still
 * accelerating. Exactly the defect the n64-verify skill lists first, found the
 * same way: a number in the overlay next to a correct-looking clip count.
 */
fm_vec3_t forge_walk_spawn(const Forge *f)
{
    const float B = (float)KILN_VOXEL_BLOCK_UNITS;

    if (f->aim.hit)
        return (fm_vec3_t){{
            f->world.offset.v[0] + ((float)f->aim.px + 0.5f) * B,
            f->world.offset.v[1] + ((float)f->aim.py + 0.5f) * B,
            f->world.offset.v[2] + ((float)f->aim.pz + 0.5f) * B }};

    int mins[3], maxs[3];
    if (kiln_voxel_bounds(&f->world, mins, maxs))
        return (fm_vec3_t){{
            f->world.offset.v[0] + ((float)(mins[0] + maxs[0]) * 0.5f + 0.5f) * B,
            /* One block clear of the highest solid block, so the drop is short
             * and lands on something rather than starting inside it. */
            f->world.offset.v[1] + ((float)maxs[1] + 1.5f) * B,
            f->world.offset.v[2] + ((float)(mins[2] + maxs[2]) * 0.5f + 0.5f) * B }};

    return f->fly_pos;
}

void forge_walk_update(Forge *f, const KilnInput *in, float dt)
{
    kiln_fpscam_update(&f->walk_cam, in, dt);

    /* ── Never leave the user falling ──────────────────────────────────
     *
     * kiln_fpscam has no floor of its own: step off the level and it accelerates
     * downwards for as long as the ROM runs, and there is no input that recovers
     * from it. In a game that is a level-design bug; in a TOOL it is a hang, and
     * the only way out is the power switch.
     *
     * So a fall past the level's own lowest block by a few blocks snaps back to
     * the spawn and SAYS so. The threshold is derived from the level's bounds
     * rather than being a constant, because a constant is wrong the moment
     * someone builds at a different height. */
    int mins[3], maxs[3];
    if (kiln_voxel_bounds(&f->world, mins, maxs)) {
        const float B = (float)KILN_VOXEL_BLOCK_UNITS;
        float floor_y = f->world.offset.v[1] + (float)mins[1] * B;
        if (f->walk_cam.pos.v[1] < floor_y - 6.0f * B) {
            kiln_fpscam_snap(&f->walk_cam, forge_walk_spawn(f), f->walk_cam.yaw, 0.0f);
            f->fell = 1;
        }
    }

    kiln_fpscam_apply(&f->walk_cam, &f->scene);
    kiln_scene_update(&f->scene);

    /* Keep the fly camera following, so leaving WALK does not teleport you back
     * across the level to wherever you entered from. */
    f->fly_pos = f->walk_cam.pos;
    f->fly_pos.v[1] += f->walk_cam.eye_height;
    f->fly_yaw = f->walk_cam.yaw;
    f->fly_pitch = f->walk_cam.pitch;
}
