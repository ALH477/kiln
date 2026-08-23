/* SPDX-License-Identifier: MIT
 *
 * forge_cam.c — the editing camera: free-fly, no gravity, no collision.
 *
 * Lifted in shape from a downstream game's own detached fly camera (built
 * for its cinematic debugger), and for the reasons that camera was built the
 * way it was rather than out of convenience:
 *
 * - **Not kiln_fpscam.** That camera probes the ground against the clip world
 *   and falls when there is none. An editor with an empty world would drop
 *   forever on the first frame — which is precisely the bug that made PLAY look
 *   like a black screen with a working HUD for its entire life.
 *
 * - **Look on the C-buttons AND the C-stick.** kiln_fpscam reads only the
 *   C-stick, and a real N64 controller does not have one — that path is dead
 *   on the pad the hardware actually ships. An editor that cannot turn
 *   around is not an editor, so both are read.
 *
 * - **Fly speed scales off the far plane.** One fixed speed is unusable
 *   across both a 512-unit room and an 8192-unit world, and it matters more
 *   here because a voxel world spans block scale to world scale by design.
 */
#include "forge.h"

#define FLY_LOOK    2.6f    /* radians/sec at full stick                   */
#define FLY_MOVE  420.0f    /* units/sec before the far-plane scale        */
#define PITCH_LIM   1.45f   /* just under 90 degrees; past it the basis flips */

void forge_cam_init(Forge *f)
{
    kiln_scene_init(&f->scene);

    /* Start outside the origin looking back at it, so a freshly seeded level is
     * in frame. A camera that boots inside the geometry is the single most
     * common way a scene reads as broken when it is not. */
    const float B = (float)KILN_VOXEL_BLOCK_UNITS;
    f->fly_pos = (fm_vec3_t){{ -6.0f * B, 8.0f * B, -6.0f * B }};
    f->fly_yaw = 0.78f;      /* facing +X+Z */
    f->fly_pitch = -0.45f;

    f->scene.fov_deg = 70.0f;
    f->scene.near_z = 8.0f;
    /* Far enough to see the whole addressable grid's diagonal, so flying out to
     * look at what you built never clips it away. The overlay prints near/far
     * because a far plane quietly under the scene's own scale is invisible in a
     * picture — the geometry simply is not drawn. */
    f->scene.far_z = 6000.0f;

    f->scene.ambient[0] = f->scene.ambient[1] = f->scene.ambient[2] = 110;
    f->scene.ambient[3] = 255;
    f->scene.clear_color = RGBA32(18, 20, 26, 255);
}

fm_vec3_t forge_cam_forward(const Forge *f)
{
    float cp = fm_cosf(f->fly_pitch);
    return (fm_vec3_t){{ fm_sinf(f->fly_yaw) * cp,
                         fm_sinf(f->fly_pitch),
                         fm_cosf(f->fly_yaw) * cp }};
}

void forge_cam_update(Forge *f, const KilnInput *in, float dt)
{
    /* Look: C-stick if it is being pushed, C-buttons otherwise. Reading both
     * unconditionally would mean a held C-button fights a resting stick. */
    float lx = 0.0f, ly = 0.0f;
    if (in->cstick_x > 20 || in->cstick_x < -20) lx = (float)in->cstick_x / 80.0f;
    if (in->cstick_y > 20 || in->cstick_y < -20) ly = (float)in->cstick_y / 80.0f;
    if (in->buttons & KILN_BTN_CR) lx += 1.0f;
    if (in->buttons & KILN_BTN_CL)  lx -= 1.0f;
    if (in->buttons & KILN_BTN_CU)    ly += 1.0f;
    if (in->buttons & KILN_BTN_CD)  ly -= 1.0f;

    f->fly_yaw   -= lx * FLY_LOOK * dt;
    f->fly_pitch += ly * FLY_LOOK * dt;
    if (f->fly_pitch >  PITCH_LIM) f->fly_pitch =  PITCH_LIM;
    if (f->fly_pitch < -PITCH_LIM) f->fly_pitch = -PITCH_LIM;

    /* Move on the main stick in the camera's own basis, plus R to sprint. The
     * scale keeps one control usable at both block and world scale. */
    float scale = f->scene.far_z / 1000.0f;
    if (scale < 0.25f) scale = 0.25f;
    if (scale > 8.0f)  scale = 8.0f;
    float sp = FLY_MOVE * scale * dt;
    if (in->buttons & KILN_BTN_R) sp *= 3.0f;

    fm_vec3_t fwd = forge_cam_forward(f);
    fm_vec3_t right = {{ fm_cosf(f->fly_yaw), 0.0f, -fm_sinf(f->fly_yaw) }};

    for (int a = 0; a < 3; a++)
        f->fly_pos.v[a] += fwd.v[a] * in->stick_y * sp + right.v[a] * in->stick_x * sp;

    /* A rises, B descends — but only when they are not the edit buttons, which
     * is why GEO uses their EDGES for place/break and this uses the held state
     * of the shoulder-free pair. In GEO, vertical movement is on the D-pad
     * instead; see forge_geo.c. */
    if (f->mode != FORGE_MODE_GEO) {
        if (in->buttons & KILN_BTN_A) f->fly_pos.v[1] += sp;
        if (in->buttons & KILN_BTN_B) f->fly_pos.v[1] -= sp;
    } else {
        if (in->buttons & KILN_BTN_DU)   f->fly_pos.v[1] += sp;
        if (in->buttons & KILN_BTN_DD) f->fly_pos.v[1] -= sp;
    }
}

void forge_cam_apply(Forge *f)
{
    fm_vec3_t fwd = forge_cam_forward(f);
    f->scene.cam_pos = f->fly_pos;
    for (int a = 0; a < 3; a++)
        f->scene.cam_target.v[a] = f->fly_pos.v[a] + fwd.v[a] * 100.0f;
    f->scene.cam_up = (fm_vec3_t){{ 0.0f, 1.0f, 0.0f }};
    kiln_scene_update(&f->scene);
}
