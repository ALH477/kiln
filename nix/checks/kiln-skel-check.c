/* SPDX-License-Identifier: MIT
 *
 * The host's skeleton, against a real rigged .t3dm.
 *
 * plat/host/src/host_t3dmodel.c used to abort on every t3d_skeleton_* call,
 * and t3dskeleton.h argued at length for why an abort beat a no-op. It now
 * implements them, which replaces that argument with a claim: the bind pose
 * the host computes is the bind pose the console computes. This file is the
 * evidence for it, and the shape of the evidence matters.
 *
 * ── What is easy to get wrong here, and invisible ──────────────────────
 * 1. The 'S' chunk is BIG-ENDIAN and its "pointers" are four-byte string-table
 *    offsets. A host pointer is eight. So the chunk cannot be cast — every
 *    field after `name` would come from the wrong offset. It is parsed, and a
 *    parse that is off by four bytes still yields plausible floats.
 *
 * 2. t3d_skeleton_update composes each bone against its PARENT in one forward
 *    pass. Composing against the previous bone instead, or skipping the
 *    compose, is indistinguishable on a one-bone rig and on any rig whose
 *    bones all sit at the origin. The fixture is two bones with the child
 *    offset 32 units up, so the composed translation is a number this file can
 *    state: bone 'arm' must land at y = 32, not y = 0.
 *
 * 3. `hasChanged` counts UP to bufferCount rather than clearing, so a settled
 *    pose keeps being rewritten until it has reached every buffer. With
 *    bufferCount 1 the count reaches zero after one update and the second
 *    update must skip the bone entirely. Clearing it eagerly looks identical
 *    here and flickers with two buffers.
 *
 * 4. t3d_quat_nlerp is normalised LERP, not slerp, and it negates the blend
 *    when the quaternions point away from each other. Blending a quaternion
 *    with its own negation is the case that separates them: nlerp must return
 *    a unit quaternion, and the naive lerp returns zero.
 *
 * ── What this check does NOT claim ─────────────────────────────────────
 * That animation works. It does not: a clip's keyframes live in a .sdata
 * sidecar the host does not stream, so t3d_anim_update keeps the console's
 * clock and leaves the bones alone. The clock is asserted here because
 * fig_skel's overlay fade and fig_skel_done read it; the pose is asserted to
 * be the BIND pose, unchanged by a clip running over it, so that this check
 * fails the day host animation lands rather than quietly passing.
 */
#include <t3d/t3dmodel.h>
#include <t3d/t3dskeleton.h>
#include <t3d/t3danim.h>
#include <kiln_engine.h>
#include <kiln_gui.h>
#include <kiln_host.h>
#include <libdragon.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* The fixed-point matrices are s16.16, so 1/65536 is one representable step.
 * Anything this check compares went through t3d_mat4_to_fixed or did not; the
 * float side is exact integers at this scale, so the tolerance is for the
 * quaternion maths only. */
#define EPS 1e-5f

static bool near_f(float a, float b) { return fabsf(a - b) <= EPS; }

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "rig.t3dm";
    const char *png  = argc > 2 ? argv[2] : "skel.png";

    fig_engine_init(RESOLUTION_320x240);
    fig_gui_init();

    T3DModel *model = t3d_model_load(path);
    CHECK(model != NULL, "t3d_model_load returned NULL");
    if (!model) return 1;

    /* ── the chunk, as parsed ──────────────────────────────────────────
     * tools/gen_skel_gltf.py builds this rig: 'hip' at the origin with no
     * parent, 'arm' 1 Blender unit above it, which --base-scale=32 turns into
     * 32 model units. Two clips, 'idle' 1.0 s and 'swing' 1.5 s.
     */
    T3DSkeleton skel = t3d_skeleton_create(model);
    const T3DChunkSkeleton *ref = skel.skeletonRef;
    CHECK(ref != NULL, "skeletonRef is NULL after t3d_skeleton_create");
    if (!ref) return 1;

    CHECK(ref->boneCount == 2, "boneCount %u, expected 2", ref->boneCount);
    if (ref->boneCount != 2) { printf("\nFAILED (%d)\n", fails + 1); return 1; }

    CHECK(ref->bones[0].name && strcmp(ref->bones[0].name, "hip") == 0,
          "bone 0 name '%s', expected 'hip'. A name that is garbage rather "
          "than wrong means the string-table offset was read at the wrong "
          "place in the bone struct.",
          ref->bones[0].name ? ref->bones[0].name : "(null)");
    CHECK(ref->bones[1].name && strcmp(ref->bones[1].name, "arm") == 0,
          "bone 1 name '%s', expected 'arm'",
          ref->bones[1].name ? ref->bones[1].name : "(null)");

    /* 0xFFFF is the console's "no parent" and t3d_skeleton_update tests for
     * exactly that value. A 0 here would make the root its own parent. */
    CHECK(ref->bones[0].parentIdx == 0xFFFF,
          "bone 0 parentIdx %u, expected 0xFFFF (no parent)",
          ref->bones[0].parentIdx);
    CHECK(ref->bones[1].parentIdx == 0, "bone 1 parentIdx %u, expected 0",
          ref->bones[1].parentIdx);
    CHECK(ref->bones[0].depth == 0 && ref->bones[1].depth == 1,
          "depths %u/%u, expected 0/1 — fig_skel builds its bone masks from "
          "these", ref->bones[0].depth, ref->bones[1].depth);

    /* The one number that proves the 48-byte bone stride and the float
     * byte order at once. Read one field early and every later float is
     * plausible nonsense. */
    CHECK(near_f(ref->bones[1].position.v[1], 32.0f),
          "bone 'arm' bind position y = %f, expected 32.0 (1 Blender unit at "
          "--base-scale=32). A wrong value here is the bone stride or the "
          "big-endian float read.", ref->bones[1].position.v[1]);
    CHECK(near_f(ref->bones[1].position.v[0], 0.0f) &&
          near_f(ref->bones[1].position.v[2], 0.0f),
          "bone 'arm' bind position (%f, ., %f), expected x and z zero",
          ref->bones[1].position.v[0], ref->bones[1].position.v[2]);
    for (int i = 0; i < 2; i++)
        CHECK(near_f(ref->bones[i].scale.v[0], 1.0f) &&
              near_f(ref->bones[i].scale.v[1], 1.0f) &&
              near_f(ref->bones[i].scale.v[2], 1.0f),
              "bone %d bind scale is not unit: (%f, %f, %f)", i,
              ref->bones[i].scale.v[0], ref->bones[i].scale.v[1],
              ref->bones[i].scale.v[2]);

    /* ── create put the bind pose into the live bones ── */
    CHECK(skel.bufferCount == 1, "bufferCount %u, expected 1 from "
          "t3d_skeleton_create", skel.bufferCount);
    CHECK(skel.bones != NULL && skel.boneMatricesFP != NULL,
          "t3d_skeleton_create left a buffer NULL");
    CHECK(near_f(skel.bones[1].position.v[1], 32.0f),
          "t3d_skeleton_reset did not copy the bind position: live bone 1 y = "
          "%f. The console copies the SRT triple as one memcpy relying on "
          "adjacency; the host copies per member, and this is the assertion "
          "that says the two agree.", skel.bones[1].position.v[1]);
    CHECK(skel.bones[0].hasChanged && skel.bones[1].hasChanged,
          "t3d_skeleton_reset left hasChanged clear, so the first "
          "t3d_skeleton_update would compute nothing");

    /* ── the composed bind pose ────────────────────────────────────────
     * This is the load-bearing one. Column-major with the translation in row
     * 3: m[3] is the position. 'hip' is identity at the origin; 'arm' must
     * come out at y = 32 BECAUSE it was composed with its parent, even though
     * its own local translation is also (0, 32, 0) — so this alone does not
     * separate "composed" from "not composed". The second half does: move the
     * hip and the arm has to follow.
     */
    t3d_skeleton_update(&skel);
    CHECK(near_f(skel.bones[0].matrix.m[3][0], 0.0f) &&
          near_f(skel.bones[0].matrix.m[3][1], 0.0f) &&
          near_f(skel.bones[0].matrix.m[3][2], 0.0f),
          "'hip' world position (%f, %f, %f), expected the origin",
          skel.bones[0].matrix.m[3][0], skel.bones[0].matrix.m[3][1],
          skel.bones[0].matrix.m[3][2]);
    CHECK(near_f(skel.bones[1].matrix.m[3][1], 32.0f),
          "'arm' world y = %f, expected 32.0",
          skel.bones[1].matrix.m[3][1]);

    /* hasChanged has now counted up to bufferCount and wrapped to 0, so a
     * second update must not touch the bone. Prove it by poisoning the matrix:
     * if the update runs, it overwrites the poison. */
    skel.bones[1].matrix.m[3][1] = -999.0f;
    t3d_skeleton_update(&skel);
    CHECK(near_f(skel.bones[1].matrix.m[3][1], -999.0f),
          "a settled bone was recomputed: hasChanged is %d after one update, "
          "expected 0. Clearing it eagerly instead of counting up to "
          "bufferCount looks identical with one buffer and flickers with two.",
          skel.bones[1].hasChanged);

    /* Move the root and the child must follow, through the forced walk of
     * children that `depth` bounds. This is what a missing compose breaks. */
    skel.bones[0].position.v[0] = 10.0f;
    skel.bones[0].hasChanged = true;
    t3d_skeleton_update(&skel);
    CHECK(near_f(skel.bones[0].matrix.m[3][0], 10.0f),
          "'hip' did not move: x = %f, expected 10.0",
          skel.bones[0].matrix.m[3][0]);
    CHECK(near_f(skel.bones[1].matrix.m[3][0], 10.0f) &&
          near_f(skel.bones[1].matrix.m[3][1], 32.0f),
          "'arm' is at (%f, %f) after moving only its parent, expected "
          "(10.0, 32.0). Its own hasChanged was clear, so reaching it at all "
          "depends on the forced walk of children bounded by `depth` — the "
          "part of t3d_skeleton_update that looks droppable and is not.",
          skel.bones[1].matrix.m[3][0], skel.bones[1].matrix.m[3][1]);

    /* The fixed-point round trip actually happened. s16.16, so 32.0 is
     * 32 * 65536 = 0x200000 in whatever the host's T3DMat4FP holds — read it
     * back through the same quantiser rather than assuming a layout. */
    {
        T3DMat4FP fp;
        t3d_mat4_to_fixed(&fp, &skel.bones[1].matrix);
        CHECK(memcmp(&fp, &skel.boneMatricesFP[1], sizeof fp) == 0,
              "boneMatricesFP[1] is not t3d_mat4_to_fixed of bone 1's matrix, "
              "so the draw would pose from a stale or unwritten matrix");
    }
    /* Restore the bind pose for the render below. */
    t3d_skeleton_reset(&skel);
    t3d_skeleton_update(&skel);

    /* ── clone ─────────────────────────────────────────────────────────
     * fig_skel clones with `false` twice (kiln_skel.c:21,82) for its blend
     * and overlay skeletons, and then blends into them. A clone that shared
     * the bone array would make every blend write through to the base pose.
     */
    T3DSkeleton clone = t3d_skeleton_clone(&skel, false);
    CHECK(clone.bones != NULL && clone.bones != skel.bones,
          "t3d_skeleton_clone shared the bone array instead of copying it");
    CHECK(clone.boneMatricesFP == NULL,
          "t3d_skeleton_clone(.., false) allocated matrices anyway");
    CHECK(clone.skeletonRef == skel.skeletonRef,
          "the clone points at a different skeleton chunk");
    CHECK(near_f(clone.bones[1].position.v[1], 32.0f),
          "the clone's bone 1 y = %f, expected the bind 32.0",
          clone.bones[1].position.v[1]);
    clone.bones[1].position.v[1] = 7.0f;
    CHECK(near_f(skel.bones[1].position.v[1], 32.0f),
          "writing the clone changed the original: aliased bone arrays");
    clone.bones[1].position.v[1] = 32.0f;

    /* ── blend ─────────────────────────────────────────────────────────
     * Endpoints first, then the midpoint, then the sign case that separates
     * nlerp from a plain lerp.
     */
    {
        T3DSkeleton a = t3d_skeleton_clone(&skel, false);
        T3DSkeleton b = t3d_skeleton_clone(&skel, false);
        T3DSkeleton r = t3d_skeleton_clone(&skel, false);
        a.bones[1].position.v[1] = 0.0f;
        b.bones[1].position.v[1] = 64.0f;

        t3d_skeleton_blend(&r, &a, &b, 0.0f);
        CHECK(near_f(r.bones[1].position.v[1], 0.0f),
              "blend at factor 0 gave %f, expected a's 0.0",
              r.bones[1].position.v[1]);
        t3d_skeleton_blend(&r, &a, &b, 1.0f);
        CHECK(near_f(r.bones[1].position.v[1], 64.0f),
              "blend at factor 1 gave %f, expected b's 64.0",
              r.bones[1].position.v[1]);
        t3d_skeleton_blend(&r, &a, &b, 0.25f);
        CHECK(near_f(r.bones[1].position.v[1], 16.0f),
              "blend at factor 0.25 gave %f, expected 16.0 — the factor is "
              "not reversed and the lerp is linear",
              r.bones[1].position.v[1]);
        CHECK(r.bones[1].hasChanged,
              "blend left hasChanged clear, so t3d_skeleton_update would not "
              "rebuild the matrix it just changed");

        /* A quaternion and its negation are the SAME rotation. A plain lerp at
         * 0.5 cancels them to zero and then normalising divides by zero; nlerp
         * negates the blend first and returns a unit quaternion. This is the
         * case kiln_pose.c reimplemented t3d_quat_nlerp for. */
        a.bones[1].rotation = (T3DQuat){{ 0.0f, 0.70710678f, 0.0f, 0.70710678f }};
        b.bones[1].rotation = (T3DQuat){{ 0.0f, -0.70710678f, 0.0f, -0.70710678f }};
        t3d_skeleton_blend(&r, &a, &b, 0.5f);
        const float *q = r.bones[1].rotation.v;
        const float len = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
        CHECK(fabsf(len - 1.0f) <= 1e-4f,
              "blending a quaternion with its own negation gave length %f, "
              "expected 1.0. A plain lerp cancels them to zero here; nlerp "
              "negates the blend when the dot product is negative.", len);

        t3d_skeleton_destroy(&a);
        t3d_skeleton_destroy(&b);
        t3d_skeleton_destroy(&r);
        CHECK(a.bones == NULL && a.skeletonRef == NULL,
              "t3d_skeleton_destroy left the skeleton usable");
    }

    /* ── the clock, without the keyframes ─────────────────────────────── */
    T3DAnim idle  = t3d_anim_create(model, "idle");
    T3DAnim swing = t3d_anim_create(model, "swing");
    CHECK(near_f(t3d_anim_get_length(&idle), 1.0f),
          "'idle' length %f, expected 1.0", t3d_anim_get_length(&idle));
    CHECK(near_f(t3d_anim_get_length(&swing), 1.5f),
          "'swing' length %f, expected 1.5 — two clips parsed from one chunk "
          "table, so a wrong stride here reads the second clip's header from "
          "inside the first's channel mappings",
          t3d_anim_get_length(&swing));
    CHECK(idle.animRef->channelsQuat == 1 && idle.animRef->channelsScalar == 0,
          "'idle' channels %u quat / %u scalar, expected 1 / 0. These set the "
          "chunk's size, which is how the next clip is found.",
          idle.animRef->channelsQuat, idle.animRef->channelsScalar);
    CHECK(idle.animRef->filePath != NULL &&
          strstr(idle.animRef->filePath, "sdata") != NULL,
          "'idle' filePath '%s' does not name a .sdata sidecar; that is where "
          "the keyframes are, and the host not reading it is the reason the "
          "pose below stays the bind pose",
          idle.animRef->filePath ? idle.animRef->filePath : "(null)");
    CHECK(idle.isPlaying && idle.isLooping,
          "a fresh clip is not playing and looping, which is what the console "
          "returns from t3d_anim_create");

    /* Non-looping: stops at the end, keeps the overshoot as its time. */
    t3d_anim_set_looping(&idle, false);
    t3d_anim_update(&idle, 0.6f);
    CHECK(near_f(t3d_anim_get_time(&idle), 0.6f) && t3d_anim_is_playing(&idle),
          "after 0.6 s of a 1.0 s clip: time %f, playing %d",
          t3d_anim_get_time(&idle), (int)t3d_anim_is_playing(&idle));
    t3d_anim_update(&idle, 0.6f);
    CHECK(!t3d_anim_is_playing(&idle),
          "a non-looping clip is still playing past its duration — "
          "fig_skel_done reads exactly this");
    CHECK(near_f(t3d_anim_get_time(&idle), 0.2f),
          "time %f after overshooting a 1.0 s clip by 0.2, expected 0.2 (the "
          "console subtracts the duration rather than clamping)",
          t3d_anim_get_time(&idle));

    /* Looping wraps and keeps going, and speed scales dt. */
    t3d_anim_set_time(&swing, 0.0f);
    t3d_anim_set_speed(&swing, 2.0f);
    t3d_anim_update(&swing, 0.5f);
    CHECK(near_f(t3d_anim_get_time(&swing), 1.0f),
          "speed 2.0 over dt 0.5 advanced to %f, expected 1.0",
          t3d_anim_get_time(&swing));
    t3d_anim_update(&swing, 0.5f);
    CHECK(near_f(t3d_anim_get_time(&swing), 0.5f) && t3d_anim_is_playing(&swing),
          "a looping clip at 1.5 s duration wrapped to %f, expected 0.5, and "
          "playing %d", t3d_anim_get_time(&swing),
          (int)t3d_anim_is_playing(&swing));

    /* fig_skel_set_phase divides by the length. The parser refuses a zero
     * duration precisely so this is safe; assert it rather than trust it. */
    CHECK(t3d_anim_get_length(&swing) > 0.0f,
          "a zero-length clip reached the engine; fig_skel_set_phase divides "
          "by this");

    /* And the staging claim: a clip running over the skeleton does NOT move
     * it. When host animation lands, this assertion is the one that fails,
     * which is the point of writing it down. */
    t3d_anim_attach(&swing, &skel);
    const float before = skel.bones[1].position.v[1];
    for (int i = 0; i < 30; i++) t3d_anim_update(&swing, 1.0f / 30.0f);
    CHECK(near_f(skel.bones[1].position.v[1], before),
          "an animation moved a bone (%f -> %f). The host does not stream "
          ".sdata keyframes, so this should be impossible — if host animation "
          "has landed, this check needs rewriting rather than deleting.",
          before, skel.bones[1].position.v[1]);

    /* ── and it draws ──────────────────────────────────────────────────
     * host_t3d.c's vertex cache holds TRANSFORMED vertices, which is the
     * semantic rigid skinning rests on: pushing a bone's matrix before loading
     * that bone's part is what poses it. The counters below are the evidence
     * that both parts loaded — a skinned model whose second part was skipped
     * draws half a mesh and looks like a modelling mistake.
     */
    FigScene scene;
    fig_scene_init(&scene);
    scene.cam_pos    = (fm_vec3_t){{ 96.0f, 40.0f, 96.0f }};
    scene.cam_target = (fm_vec3_t){{  0.0f, 24.0f,  0.0f }};
    scene.fov_deg    = 50.0f;
    scene.near_z     = 5.0f;
    scene.far_z      = 400.0f;
    scene.clear_color = RGBA32(0x08, 0x0A, 0x10, 0xFF);
    scene.ambient[0] = scene.ambient[1] = scene.ambient[2] = 0x50;
    scene.ambient[3] = 0xFF;
    scene.light_color[0] = 0xFF; scene.light_color[1] = 0xF4;
    scene.light_color[2] = 0xD0; scene.light_color[3] = 0xFF;
    scene.light_dir = (fm_vec3_t){{ 0.5f, 0.75f, 0.4f }};
    scene.light_count = 1;
    fig_scene_update(&scene);

    FigTransform xf;
    fig_transform_init(&xf);

    fig_frame_begin();
      fig_scene_begin(&scene);
        fig_transform_push(&xf);
          t3d_model_draw_skinned(model, &skel);
        fig_transform_pop();
      fig_gui_begin();
        fig_gui_rect(0, 0, 320, 12, RGBA32(0x10, 0x12, 0x18, 0xFF));
        fig_gui_text(6, 9, RGBA32(0x00, 0xF5, 0xD4, 0xFF), "KILN SKEL");
        fig_gui_text(6, 232, RGBA32(0xA0, 0xA0, 0xB0, 0xFF),
                      "%u bones  bind pose  %u clips", ref->boneCount, 2u);
      fig_gui_end();
    fig_frame_end();

    const FigHostT3DCounters *t = fig_host_t3d_counters();
    printf("  drew: loads %u verts %u submitted %u drawn %u culled %u clipped %u\n",
           t->vert_loads, t->verts, t->tris_submitted, t->tris_drawn,
           t->tris_culled, t->tris_clipped);
    /* Two parts, one per bone, so two loads — and the one number that says the
     * per-part matrix push did not collapse them. host_t3d.c records this
     * fixture's shape: "part 0 has matrixIdx 1 and draws nothing, part 1 has
     * matrixIdx 0 and draws 72 indices across both slices". */
    CHECK(t->vert_loads == 2, "%u vertex loads, expected 2 (one per bone's "
          "part). One would mean a part was skipped; the partial-load `continue`"
          " must come AFTER t3d_vert_load, not before it.", t->vert_loads);
    CHECK(t->tris_submitted == 24, "%u triangles submitted, expected 24 (72 "
          "indices / 3)", t->tris_submitted);
    CHECK(t->tris_drawn > 0, "nothing produced pixels, so the bind pose put "
          "the mesh off screen or behind the camera");

    fig_host_stats(stdout, 5);
    CHECK(fig_host_capture(png) == 0, "could not write %s", png);

    t3d_anim_destroy(&idle);
    t3d_anim_destroy(&swing);
    t3d_skeleton_destroy(&clone);
    t3d_skeleton_destroy(&skel);
    t3d_model_free(model);

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("a rigged .t3dm poses its bind skeleton on the host\n");
    return 0;
}
