/* SPDX-License-Identifier: MIT
 *
 * kiln_fpscam's handedness, asserted against the renderer's own view basis.
 *
 * The camera is right-handed: t3d_viewport_look_at builds side = forward x up
 * and looks down -Z of view space, in Tiny3D (t3dmath.c) and in the host shim
 * alike, so an eye looking down +Z has screen-right at -X. kiln_fpscam used to
 * say "right" was +X at yaw 0 and turned "right" by increasing yaw — both
 * toward screen-LEFT. Stick right strafed left and C-right turned left, on
 * console as on the host, in examples/fps and in Forge's WALK mode.
 *
 * Nothing about that is visible in a still frame, and each consumer can paper
 * over it by negating an axis at the call site, which is how a convention
 * error survives. So this asks the renderer where screen-right is, at several
 * headings, and requires the pad to agree:
 *
 *   strafe   stick right moves the eye along screen-right
 *   turn     C right turns the forward vector toward screen-right
 *   right    kiln_fpscam_right() IS screen-right
 *   forward  stick up moves along kiln_fpscam_forward; C up looks up
 */
#include <kiln_fpscam.h>
#include <kiln_clip.h>
#include <libdragon.h>
#include <t3d/t3d.h>

#include <stdio.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static float dot(fm_vec3_t a, fm_vec3_t b)
{
    return a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2];
}

/* The world direction that projects to screen +X: view-space x is
 * m[0][0]*x + m[1][0]*y + m[2][0]*z + m[3][0] (column-major), and the
 * projection keeps its sign. */
static fm_vec3_t screen_right(const KilnFpsCam *cam)
{
    T3DViewport vp = { 0 };
    const fm_vec3_t f = kiln_fpscam_forward(cam);
    const fm_vec3_t target = {{ cam->pos.v[0] + f.v[0] * 100, cam->pos.v[1] + f.v[1] * 100,
                                cam->pos.v[2] + f.v[2] * 100 }};
    const fm_vec3_t up = {{ 0, 1, 0 }};
    t3d_viewport_look_at(&vp, &cam->pos, &target, &up);
    return (fm_vec3_t){{ vp.matCamera.m[0][0], vp.matCamera.m[1][0], vp.matCamera.m[2][0] }};
}

static void fresh(KilnFpsCam *cam, float yaw)
{
    kiln_fpscam_init(cam);
    cam->gravity = 0;                         /* no world: stay at eye height */
    kiln_fpscam_snap(cam, (fm_vec3_t){{ 10, 20, 30 }}, yaw, 0);
}

int main(void)
{
    kiln_clip_set_world(NULL, 0);
    const float YAWS[] = { 0.0f, 1.0f, 2.5f, -2.0f };

    for (unsigned i = 0; i < sizeof YAWS / sizeof YAWS[0]; i++) {
        const float yaw = YAWS[i];
        KilnFpsCam cam;

        fresh(&cam, yaw);
        const fm_vec3_t right = screen_right(&cam);
        const fm_vec3_t kr = kiln_fpscam_right(&cam);
        CHECK(dot(kr, right) > 0.99f,
              "yaw %.2f: kiln_fpscam_right is (%.2f %.2f %.2f) but screen-right is (%.2f %.2f %.2f)",
              (double)yaw, (double)kr.v[0], (double)kr.v[1], (double)kr.v[2],
              (double)right.v[0], (double)right.v[1], (double)right.v[2]);

        /* Strafe: stick right. */
        fm_vec3_t before = cam.pos;
        kiln_fpscam_update(&cam, &(KilnInput){ .stick_x = 1.0f }, 1.0f / 60.0f);
        fm_vec3_t moved = {{ cam.pos.v[0] - before.v[0], 0, cam.pos.v[2] - before.v[2] }};
        CHECK(dot(moved, right) > 0.0f,
              "yaw %.2f: stick right moved the eye (%.2f %.2f) — screen-LEFT, against screen-right (%.2f %.2f)",
              (double)yaw, (double)moved.v[0], (double)moved.v[2],
              (double)right.v[0], (double)right.v[2]);

        /* Forward: stick up moves along the look direction. */
        fresh(&cam, yaw);
        before = cam.pos;
        const fm_vec3_t fwd = kiln_fpscam_forward(&cam);
        kiln_fpscam_update(&cam, &(KilnInput){ .stick_y = 1.0f }, 1.0f / 60.0f);
        moved = (fm_vec3_t){{ cam.pos.v[0] - before.v[0], 0, cam.pos.v[2] - before.v[2] }};
        CHECK(dot(moved, fwd) > 0.0f, "yaw %.2f: stick up moved the eye backwards", (double)yaw);

        /* Turn: C right swings forward toward screen-right. */
        fresh(&cam, yaw);
        kiln_fpscam_update(&cam, &(KilnInput){ .cstick_x = 1.0f }, 1.0f / 60.0f);
        const fm_vec3_t turned = kiln_fpscam_forward(&cam);
        CHECK(dot(turned, right) > 0.0f,
              "yaw %.2f: C right turned the view toward screen-LEFT (yaw %.2f -> %.2f)",
              (double)yaw, (double)yaw, (double)cam.yaw);

        /* Look: C up raises the view. */
        fresh(&cam, yaw);
        kiln_fpscam_update(&cam, &(KilnInput){ .cstick_y = 1.0f }, 1.0f / 60.0f);
        CHECK(kiln_fpscam_forward(&cam).v[1] > 0.0f, "yaw %.2f: C up looked down", (double)yaw);
    }

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("kiln_fpscam: stick right strafes and C right turns toward the renderer's screen-right\n");
    return 0;
}
