/* SPDX-License-Identifier: MIT
 *
 * forge_hud.c — the numbers, and specifically the ones that are red when wrong.
 *
 * This repo's verification story rests on a single principle: a missing thing
 * has to be reported by something, because on screen it is indistinguishable
 * from a camera pointed elsewhere. Four of the five worst defects in PetaByte
 * Madness passed `nix build` and `nix flake check` and were found by a number in
 * an overlay — `clip 0`, an `eye Y` next to drawn brushes, a `veil-pal 0/3`.
 *
 * So every capacity in this editor has a gauge and every gauge goes red at the
 * value that means the level is lying to you:
 *
 *   chunks n/24   red at the cap: further blocks are being REFUSED
 *   mesh n%       red at 100: geometry exists that is not being drawn
 *   quads         red when a chunk overflowed its scratch
 *   brushes n/512 red at 0 in WALK (nothing to stand on) or over the cap
 *   store         red when the last save or load did not succeed
 *
 * Text is alphanumeric plus '.' '-' '/' and space only. FONT_BUILTIN_DEBUG_MONO
 * has no '@' glyph and renders it as '0', so a field reading "2@5" comes out as
 * "205" — which reads as a coordinate.
 */
#include "forge.h"

static const char *MODE_NAME[FORGE_MODE_COUNT] = {
    "GEO", "WALK", "PAINT", "ENT", "LIGHT", "CAM",
};

#define OK_COL   RGBA32(220, 220, 220, 255)
#define DIM_COL  RGBA32(140, 140, 140, 255)
#define BAD_COL  RGBA32(255,  80,  70, 255)
#define HOT_COL  RGBA32(255, 210,  70, 255)

void forge_hud_draw(Forge *f)
{
    int chunks = kiln_voxel_chunk_count(&f->world);
    uint32_t mesh_pct = kiln_voxmesh_arena_used_pct(&f->arena);

    color_t chunk_col = chunks >= KILN_VOXEL_MAX_CHUNKS ? BAD_COL : OK_COL;
    color_t mesh_col  = (f->arena_overflow || mesh_pct >= 100) ? BAD_COL : OK_COL;

    kiln_gui_text(6, 8, HOT_COL, "FORGE %s  blk %d  %.0f fps",
                 MODE_NAME[f->mode], f->block, f->fps);

    kiln_gui_text(6, 20, chunk_col, "chunks %d/%d  solid %lu",
                 chunks, KILN_VOXEL_MAX_CHUNKS,
                 (unsigned long)kiln_voxel_solid_count(&f->world));

    /* Quads AND the arena percentage. The percentage alone read "mesh 0%" over
     * a room that was plainly drawn — greedy merging is effective enough that a
     * whole room is ~30 quads out of a 3072-quad arena, so the honest reading
     * rounds to zero and looks exactly like nothing was meshed. A gauge that
     * says 0 when the answer is "yes, and cheaply" is a gauge nobody will
     * believe the next time it says 0 for a real reason. */
    kiln_gui_text(6, 30, mesh_col, "quads %lu  arena %lu%%",
                 (unsigned long)f->quads_drawn, (unsigned long)mesh_pct);

    if (f->remesh_overflow)
        kiln_gui_text(150, 30, BAD_COL, "OVER by %d", f->remesh_overflow);

    /* The clip world, printed in both modes rather than only in WALK: knowing
     * how many brushes the level WILL export as is the number you want while
     * building, not after. Red at zero only in WALK, where an empty clip world
     * means falling forever; an empty world in GEO is just an empty world. */
    color_t box_col = OK_COL;
    if (f->mode == FORGE_MODE_WALK && f->boxes_used == 0) box_col = BAD_COL;
    if (f->box_overflow) box_col = BAD_COL;
    kiln_gui_text(6, 40, box_col, "clip %lu/%d%s%s", (unsigned long)f->boxes_used,
                 FORGE_MAX_BOXES, f->box_overflow ? " OVER" : "",
                 f->fell ? " FELL-RESPAWNED" : "");

    /* Where the camera actually is, and the frustum it built the scene from.
     * "the camera is inside the geometry" is not answerable from a picture. */
    kiln_gui_text(6, 50, DIM_COL, "eye %.0f %.0f %.0f  near %.0f far %.0f",
                 (double)f->scene.cam_pos.v[0], (double)f->scene.cam_pos.v[1],
                 (double)f->scene.cam_pos.v[2],
                 (double)f->scene.near_z, (double)f->scene.far_z);

    /* The aimed block in BLOCK coordinates, which is what you type into a
     * generator or a comment. World units are derivable; block indices are what
     * the level is authored in. */
    if (f->aim.hit && (f->mode == FORGE_MODE_GEO || f->mode == FORGE_MODE_ENT))
        kiln_gui_text(6, 60, DIM_COL, "aim %d %d %d t%d  put %d %d %d  d %.0f",
                     f->aim.x, f->aim.y, f->aim.z, f->aim.block,
                     f->aim.px, f->aim.py, f->aim.pz, (double)f->aim.dist);
    else if (f->mode == FORGE_MODE_GEO || f->mode == FORGE_MODE_ENT)
        kiln_gui_text(6, 60, DIM_COL, "aim -");

    /* Storage. `cart` and `store` are printed every frame and not behind a key,
     * for the reason pm_cine prints its pose continuously: it means any
     * screenshot of the editor carries the answer to "could this have saved?" */
    color_t st_col = (f->last_store == KILN_STORE_OK) ? OK_COL : BAD_COL;
    kiln_gui_text(6, FORGE_SCREEN_H - 30, kiln_store_writable() ? OK_COL : BAD_COL,
                 "cart %s  store %s  bus %s",
                 kiln_store_cart_name(), kiln_store_kind_name(),
                 kiln_store_bus_name());
    if (f->last_action)
        kiln_gui_text(6, FORGE_SCREEN_H - 20, st_col, "%s %s",
                     f->last_action, kiln_store_status_name(f->last_store));

    /* The control reminder stays on screen. An editor whose bindings have to be
     * remembered from a README is an editor used with a laptop open next to the
     * television, and the whole argument for being on the console is that the
     * television is where your attention is. */
    /* One line per mode. It stays on screen rather than living in a README,
     * because an editor whose bindings have to be remembered is an editor used
     * with a laptop open next to the television — and the whole argument for
     * being on the console is that the television is where your attention is. */
    static const char *const HELP[FORGE_MODE_COUNT] = {
        [FORGE_MODE_GEO]   = "A put B dig Z+A fill LR mode START save",
        [FORGE_MODE_WALK]  = "stick walk R run LR mode START save",
        [FORGE_MODE_PAINT] = "dpad move A draw B pick Z veil LR mode",
        [FORGE_MODE_ENT]   = "A place B remove dpad class LR mode",
        [FORGE_MODE_LIGHT] = "C-ud field dpad edit A fog R clear",
        [FORGE_MODE_CAM]   = "A key B del Z play dpad scrub LR mode",
    };
    kiln_gui_text(6, FORGE_SCREEN_H - 10, DIM_COL, "%s", HELP[f->mode]);
}
