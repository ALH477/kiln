/* SPDX-License-Identifier: MIT
 *
 * Forge — a voxel level editor that runs on the console.
 *
 * `nix build .#forge` -> forge.z64. Put it on the flashcart's SD card once; from
 * then on the levels are DATA on that card and editing costs no rebuild at all.
 * That is the property the whole design is arranged around, and it is what makes
 * hardware iteration viable on a cart with no USB: see Forge/selftest for the
 * probe that establishes whether this machine's SD write path works, and
 * engine/src/kiln/kiln_store.h for the three-rung fallback when it does not.
 *
 * Modes: GEO places and breaks blocks from a free-fly camera; WALK installs the
 * greedy-meshed boxes as the real clip world and hands the pad to kiln_fpscam, so
 * a doorway's width is judged by walking through it. L+R cycles.
 *
 * ── Read the numbers, not the picture ──────────────────────────────────
 *
 * Everything with a capacity has a gauge, and every gauge goes red at the value
 * that means the editor is lying to you. That is not decoration: four of the
 * five worst defects this engine has had passed `nix build` and `nix flake
 * check` cleanly and were found by a number in an overlay. An editor is the
 * worst possible place for a silent capacity, because the symptom is a block
 * that does not appear and the user's conclusion is that they mis-aimed.
 */
#include <malloc.h>

#include "forge.h"

static Forge g_forge;

int main(void)
{
    kiln_engine_init(RESOLUTION_320x240);
    kiln_gui_init();
    joypad_init();

    Forge *f = &g_forge;
    memset(f, 0, sizeof *f);
    f->block = 1;
    f->mode = FORGE_MODE_GEO;
    f->last_store = KILN_STORE_OK;
    f->ent_sel = f->ent_hover = -1;
    f->key_sel = -1;
    f->paint_colour = 8;

    /* Light defaults that make an untouched level READABLE rather than correct.
     * A key from above and slightly to one side, a dim fill from the opposite
     * side, and enough ambient that an unlit face is not black — because the
     * first thing anyone does is build, not light, and geometry you cannot see
     * is indistinguishable from geometry that is not there. */
    f->key_yaw = 0.9f;   f->key_pitch = -0.9f;  f->key_level = 200;
    f->fill_yaw = -2.2f; f->fill_pitch = -0.3f; f->fill_level = 90;
    f->ambient = 110;
    f->fog_near = 600.0f;
    f->fog_far = 2400.0f;
    f->cine_duration = 8.0f;

    /* Storage before anything else, so the HUD can report it from frame one and
     * a session is never spent building into a save that was never going to
     * work. The preference order walks SD -> save chip -> read-only ROM; under
     * an emulator it lands on the last of those and says so. */
    kiln_store_init(KILN_STORE_CART_SD);

    /* The vertex arena, uncached because the RSP DMAs out of it. One allocation
     * for the whole session: this module never mallocs again, so there is no
     * frame on which a remesh can fail for want of memory. */
    T3DVertPacked *verts = malloc_uncached(sizeof(T3DVertPacked) * FORGE_ARENA_ENTRIES);
    assertf(verts != NULL, "forge: %d KB vertex arena did not fit",
            (int)(sizeof(T3DVertPacked) * FORGE_ARENA_ENTRIES / 1024));
    kiln_voxmesh_arena_init(&f->arena, verts, FORGE_ARENA_ENTRIES);

    if (kiln_voxatlas_init(&f->atlas) != 0)
        assertf(0, "forge: the 2 KB CI4 atlas did not fit");

    forge_cam_init(f);
    forge_light_apply(f);

    /* Prefer the level already on the card. A tool that opens empty every boot
     * asks the user to remember to load, and the one time they forget they build
     * for twenty minutes on top of nothing and save over the real thing. */
    int load_st = forge_io_load(f);
    if (load_st != KILN_STORE_OK) {
        forge_io_seed(f);
        /* Distinguish "nothing saved yet" from "something is there and I could
         * not read it". Only the first is normal, and the first version reported
         * both as a cheerful `new ok` — which is how a ROM that had never mounted
         * its filesystem at all looked exactly like a clean first boot. A status
         * line that cannot tell those apart is worse than none: it is the reason
         * the defect took a capture to notice rather than a glance. */
        if (load_st == KILN_STORE_ENOENT || load_st == KILN_STORE_ENOINIT) {
            f->last_action = "new";
            f->last_store = KILN_STORE_OK;
        } else {
            f->last_action = "load";
            f->last_store = load_st;    /* red, and names the step */
        }
    }

    /* Boot straight into a mode, for capture. WALK needs the level loaded and
     * the clip world installed, so this has to happen AFTER the load above —
     * entering WALK over an empty world is the falling-forever case, and it
     * would look like the mode being broken rather than the order being wrong. */
#ifdef FORGE_BOOT_MODE
    f->mode = FORGE_BOOT_MODE;
    if (f->mode == FORGE_MODE_WALK) forge_walk_enter(f);
#endif

    uint64_t last_ms = get_ticks_ms();

    while (1) {
        uint64_t now = get_ticks_ms();
        float dt = (float)(now - last_ms) / 1000.0f;
        last_ms = now;
        /* Clamp: a seek, a long remesh or an emulator hitch otherwise hands the
         * camera a dt that teleports it across the level. */
        if (dt > 0.1f) dt = 0.1f;
        if (dt > 0.0f) f->fps = f->fps * 0.9f + (1.0f / dt) * 0.1f;

        kiln_input_update();
        const KilnInput *in = kiln_input_get(0);

        /* L+R cycles mode — a chord, so neither shoulder alone can change mode
         * while it is doing its own job (R sprints, L picks a block type). Same
         * reasoning as pm_cine's arm chord. */
        const int mode_changed =
            (in->buttons & KILN_BTN_L) && (in->edges & KILN_BTN_R);
        if (mode_changed) {
            f->mode = (ForgeMode)((f->mode + 1) % FORGE_MODE_COUNT);
            if (f->mode == FORGE_MODE_WALK) forge_walk_enter(f);
        }

        /* Independent of the chord, and deliberately so. This used to be the
         * `else if` of the mode test, which meant START did nothing at all
         * while L was held -- and L is held for the whole of the L+R chord, so
         * "cycle to the mode I want, then save" failed silently for whoever
         * had not let go yet. A save button that sometimes does nothing is
         * worse than no save button. */
        if (in->edges & KILN_BTN_START) forge_io_save(f);

        /* The per-mode update still skips the frame the mode changed on, so a
         * mode never sees the chord's own edges as its input. */
        if (!mode_changed) {
            switch (f->mode) {
            case FORGE_MODE_GEO:
                forge_cam_update(f, in, dt);
                forge_geo_update(f, in);
                forge_cam_apply(f);
                break;
            case FORGE_MODE_WALK:
                forge_walk_update(f, in, dt);
                break;
            case FORGE_MODE_PAINT:
                /* The camera does NOT move in PAINT. The D-pad is the texel
                 * cursor and the C buttons pick colour and tile, so there is
                 * nothing left to fly with — and holding the view still is what
                 * lets you judge a tile against the geometry it is on. */
                forge_paint_update(f, in);
                forge_cam_apply(f);
                break;
            case FORGE_MODE_ENT:
                forge_cam_update(f, in, dt);
                forge_ent_update(f, in);
                forge_cam_apply(f);
                break;
            case FORGE_MODE_LIGHT:
                /* Also static: the D-pad is aiming a light. Standing still while
                 * you aim it is the point — a light judged while the camera
                 * moves is a light judged against a moving target. */
                forge_light_update(f, in);
                forge_cam_apply(f);
                break;
            case FORGE_MODE_CAM:
                forge_cine_update(f, in, dt);
                /* The shot takes the camera only while playing; otherwise you
                 * fly. Unconditional call, conditional inside — pm_cine's shape,
                 * so the call site needs no branch. */
                if (!(f->cine_playing && forge_cine_override_camera(f)))
                    forge_cam_apply(f);
                break;
            default: break;
            }
        }

        kiln_frame_begin();
        kiln_scene_begin(&f->scene);
        forge_geo_draw(f);

        kiln_gui_begin();
        /* Every spatial overlay draws in the 2D pass through kiln_scene_project,
         * so none of them can perturb the frame they describe and all stay
         * visible through geometry — which for an edit cursor, an entity gizmo
         * and a camera path is the point rather than a compromise. */
        switch (f->mode) {
        case FORGE_MODE_GEO:   forge_geo_draw_overlay(f); break;
        case FORGE_MODE_ENT:   forge_ent_draw3d(f);  forge_ent_draw(f);   break;
        case FORGE_MODE_LIGHT: forge_light_draw3d(f); forge_light_draw(f); break;
        case FORGE_MODE_CAM:   forge_cine_draw3d(f); forge_cine_draw(f);  break;
        case FORGE_MODE_PAINT: forge_paint_draw(f); break;
        default: break;
        }
        forge_hud_draw(f);
        kiln_gui_end();
        kiln_frame_end();
    }
}
