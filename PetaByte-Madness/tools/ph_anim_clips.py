#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""ph_anim_clips.py — named animation clips for Dr. Horner's rig.

Pure choreography, no export mechanics: a list of (frame, pose) keyframes per
clip, in ph_rig.py's joint-angle convention (degrees, corrective angles away
from the slumped bind pose — zero is the standing character exactly as
delivered, not a T-pose). `ph_rig_export.py` turns this into the per-bone
keyframe tracks a glTF action needs; nothing here touches Blender or JSON.

Frames are 1:1 with seconds*60 — the importer resamples every action to a
fixed 60 Hz regardless of how it was authored (kilnlib.make_action's own
comment), so treating the authoring timeline as already-60fps means a frame
count IS a duration and there is no separate fps to keep in sync.

SIT / RECLINE / LIE below are copied verbatim from ph_intake_anim.py's own
pose tables, not imported — that module also pulls in dank_lab_gen and
lab_preview (a numpy renderer with an imageio/PIL preview path) purely to
build its GIF/MP4 review artefact, none of which this file's JSON export
needs. They are the proven "sits on the end of the slab, then leans back
into it" progression already hand-verified for pm_intake.c's original rigid
sequence, and `climb_in` below is that exact progression, just re-timed onto
this file's frame convention. LIE is deliberately the clip's final pose — it
is also the pose pm_intake.c's MOTOR/IN beats expect to find him already in,
so a one-shot clip that freezes on its last frame (same behaviour every
other one-shot in this codebase relies on — see pm_arrival.c's
"shout"/"slash"/"fire") hands off with no seam.

── Easing lives in the keys ────────────────────────────────────────────────
kilnlib.make_action forces LINEAR interpolation on every F-curve, because the
importer resamples at a fixed 60 Hz and Blender's Bezier handles would bake
overshoot into the result. The house answer, which kilnlib.ease() exists for
and which the goblin family already uses, is to put the curve in the KEYS:
sample an eased curve at intermediate frames and let the straight lines
between them approximate it.

Horner never got that treatment. Every clip below used to be two to five
poses with nothing between them, so every motion he had crossed its whole
range at a constant speed and stopped dead — which is precisely the
"programmer animation" kilnlib.ease's own docstring describes. For scale, the
other rig-JSON character in this game keys its walk every 6 frames, nine keys
per bone; Horner's walk had three.

So SOURCE_CLIPS below carries a third field per key — the easing used to
ARRIVE at that pose — and `expand()` samples it into the dense linear keys
that ship. CLIPS stays the same shape it always was, so ph_rig_export.py
needs no changes and its --verify check covers every sampled key for free.

── What must not move ──────────────────────────────────────────────────────
Clip LENGTHS are load-bearing outside this file and are unchanged:

  * pm_intake.c's T_SIT_DOWN / T_STAND_UP are 0.75f each, with a comment
    saying they match these clips' authored 45 frames. Those two constants
    feed INTRO_T, INTRO_T sets the intake camera's key times, and the camera
    keys are what pm_cine_lint validates. Re-timing a clip here would silently
    walk the whole cutscene's camera.
  * climb_in's final pose is LIE exactly, for the handoff described above.
  * Looping clips open and close on the same pose.

Adding in-between keys touches none of that: same first pose, same last pose,
same frame count.
"""

# ── easing ──────────────────────────────────────────────────────────────────
# Duplicated from tools/blender/kilnlib.py rather than imported, for the same
# reason ph_rig_export.py duplicates srgb(): this file runs under a bare
# python3 with no Blender (that is what makes the rig testable at all), and
# kilnlib imports bpy at module scope. Keep the curves identical to kilnlib's —
# they are the same four modes by the same formulas.
def ease(t, mode="inout"):
    """Remap 0..1 through an easing curve.

    "in" accelerates, "out" decelerates, "inout" does both, and "over"
    decelerates past the target and settles back — the one that makes a limb
    feel like it has mass. `None` means a straight line, and emits no
    intermediate keys at all.
    """
    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
    if mode == "in":
        return t * t
    if mode == "out":
        return 1.0 - (1.0 - t) * (1.0 - t)
    if mode == "over":
        s = 1.70158 * 0.6
        u = t - 1.0
        return u * u * ((s + 1.0) * u + s) + 1.0
    return t * t * (3.0 - 2.0 * t)      # inout / smoothstep


# Frames between sampled keys inside an eased segment. Six matches what the
# machine centaur's exporter already emits for its own cycles, and at 60 Hz
# the straight lines between six-frame samples are not distinguishable from
# the curve they approximate. It is a size/fidelity trade and nothing else:
# halving it doubles the keys in the JSON.
STEP = 6


def _lerp_pose(a, b, f):
    """Blend two partial poses. A joint absent from either side reads as zero,
    which is this convention's neutral — the same defaulting ph_rig.py's own
    pose_at() does per call, and what build_channels() relies on."""
    out = {}
    for j in set(a) | set(b):
        pa = a.get(j, (0.0, 0.0, 0.0))
        pb = b.get(j, (0.0, 0.0, 0.0))
        out[j] = tuple(pa[k] + (pb[k] - pa[k]) * f for k in range(3))
    return out


def expand(keyframes, step=STEP):
    """[(frame, pose, ease), ...] -> dense [(frame, pose), ...] linear keys.

    The first key's easing is ignored (nothing precedes it to ease from).
    A segment whose easing is None is emitted as its endpoints alone, so a
    genuinely linear move costs nothing — and a hold between two identical
    poses is left as a hold rather than being padded with copies of itself.
    """
    out = [(keyframes[0][0], keyframes[0][1])]
    for i in range(1, len(keyframes)):
        f0, p0 = keyframes[i - 1][0], keyframes[i - 1][1]
        f1, p1 = keyframes[i][0], keyframes[i][1]
        mode = keyframes[i][2] if len(keyframes[i]) > 2 else None
        span = f1 - f0
        if mode is not None and span > step:
            for f in range(f0 + step, f1, step):
                u = (f - f0) / float(span)
                out.append((f, _lerp_pose(p0, p1, ease(u, mode))))
        out.append((f1, p1))
    return out


# ── poses ───────────────────────────────────────────────────────────────────
# Neutral / bind pose — the character exactly as patrick_horner_gen.py
# delivers him, already slumped.
STAND = {}

# Verbatim copies of ph_intake_anim.py's SIT / RECLINE / LIE — see the file
# comment above for why these are copied rather than imported. Keep these in
# sync with ph_intake_anim.py by hand if that file's choreography ever moves.
SIT = {
    "hip_R": (-88, 0, 0), "hip_L": (-88, 0, 0),
    "kne_R": (84, 0, 0), "kne_L": (84, 0, 0),
    "ank_R": (-8, 0, 0), "ank_L": (-8, 0, 0),
    "spine": (4, 0, 0), "chest": (5, 0, 0),
    "neck": (3, 0, 0), "head": (6, 0, 0),
    "clav_R": (0, 0, -6), "clav_L": (0, 0, 6),
}
RECLINE = {
    "hip_R": (-36, 0, 0), "hip_L": (-36, 0, 0),
    "kne_R": (34, 0, 0), "kne_L": (34, 0, 0),
    "spine": (1, 0, 0), "chest": (1, 0, 0),
    "neck": (-4, 0, 0), "head": (-2, 0, 0),
    "clav_R": (0, 0, -8), "clav_L": (0, 0, 8),
}
LIE = {
    "spine": (-3, 0, 0), "chest": (-4, 0, 0),
    "neck": (-9, 0, 0), "head": (-7, 0, 0),
    "ank_R": (-6, 0, 0), "ank_L": (-6, 0, 0),
    "clav_R": (0, 0, -5), "clav_L": (0, 0, 5),
}

# He sits on the slab and cannot make himself start. Head bows over the whole
# hold rather than the hold being 108 frames of a statue — see climb_in.
SIT_BOW = dict(
    SIT,
    spine=(9, 0, 0), chest=(11, 0, 0), neck=(8, 0, 0), head=(14, 0, 0),
    clav_R=(-12, 0, -6), clav_L=(-12, 0, 6),
    elb_R=(18, 0, 0), elb_L=(18, 0, 0),
)

# Both hands rise toward the face; head bows into them. A hold pose for
# idle_distraught's loop, not a one-shot — this plays continuously while he
# is just standing there being unable to start.
DISTRAUGHT = {
    "clav_R": (-95, 0, -8), "clav_L": (-95, 0, 8),
    "elb_R": (105, 0, 0),   "elb_L": (105, 0, 0),
    "wri_R": (20, 0, 0),    "wri_L": (20, 0, 0),
    "spine": (5, 0, 0), "chest": (7, 0, 0),
    "neck": (9, 0, 0), "head": (13, 0, 0),
}
# One shuddering breath at the top of the hold. Small on purpose: the read is
# that he is still, not that he is performing being upset.
DISTRAUGHT_BREATH = dict(
    DISTRAUGHT,
    clav_R=(-99, 0, -6), clav_L=(-99, 0, 6),
    elb_R=(110, 0, 0), elb_L=(110, 0, 0),
    spine=(8, 0, 0), chest=(11, 0, 0),
    neck=(12, 0, 0), head=(17, 0, 0),
)

# ── the walk ────────────────────────────────────────────────────────────────
# Sign conventions, read off SIT above so they are not guessed: a NEGATIVE hip
# is the thigh swung forward (SIT is -88), a POSITIVE knee is the heel drawn
# back (SIT is +84), and a NEGATIVE clavicle is the arm forward (DISTRAUGHT
# reaches the face at -95). The clavicle's Z is the arm-away-from-torso axis
# and is negative on the right, positive on the left, in every pose in this
# file — the sign that, flipped, buries both arms inside the silhouette.
#
# WALK_A and WALK_B are the two CONTACT poses, one stride apart and mirrored.
WALK_A = {
    "hip_R": (-22, 0, 0), "kne_R": (18, 0, 0), "ank_R": (6, 0, 0),
    "hip_L": (18, 0, 0),  "kne_L": (26, 0, 0), "ank_L": (-4, 0, 0),
    "clav_R": (16, 0, -4), "elb_R": (12, 0, 0),
    "clav_L": (-16, 0, 4), "elb_L": (12, 0, 0),
    "spine": (2, 0, -3), "chest": (2, 0, 3),
}
WALK_B = {
    "hip_L": (-22, 0, 0), "kne_L": (18, 0, 0), "ank_L": (6, 0, 0),
    "hip_R": (18, 0, 0),  "kne_R": (26, 0, 0), "ank_R": (-4, 0, 0),
    "clav_L": (16, 0, 4), "elb_L": (12, 0, 0),
    "clav_R": (-16, 0, -4), "elb_R": (12, 0, 0),
    "spine": (2, 0, 3), "chest": (2, 0, -3),
}

# The PASSING poses, and the reason this walk needed rebuilding rather than
# just easing. A cycle of contact -> contact alone has no pose where the legs
# are doing different things: interpolating WALK_A to WALK_B puts BOTH hips at
# -2 and BOTH knees at 22 exactly halfway, so the legs pass through each other
# in an identical half-bent pose and neither foot ever lifts far enough to
# clear the floor. That is the skate every two-pose walk has.
#
# A passing pose breaks that symmetry: the support leg is straight and takes
# the body's weight, the swing leg's knee comes up high enough to clear, its
# toe lifts, and the arms are at the middle of their swing rather than at an
# extreme. Between WALK_A and WALK_B the LEFT leg is the one travelling
# forward, so PASS_A supports on the right.
PASS_A = {
    "hip_R": (-2, 0, 0),  "kne_R": (6, 0, 0),   "ank_R": (-2, 0, 0),
    "hip_L": (-8, 0, 0),  "kne_L": (50, 0, 0),  "ank_L": (-14, 0, 0),
    "clav_R": (0, 0, -5), "elb_R": (15, 0, 0),
    "clav_L": (0, 0, 5),  "elb_L": (15, 0, 0),
    "spine": (3, 0, 0), "chest": (3, 0, 0),
    # The pelvis drops toward the unsupported side through single support.
    # Two degrees: with no root TRANSLATION available (the rig JSON carries
    # rotation only, and the walk's travel is pm_intake.c's KilnTransform), a
    # small roll is the whole of the weight shift, so it stays subtle enough
    # to read as weight rather than as a limp.
    "root": (0, 0, -2),
}
PASS_B = {
    "hip_L": (-2, 0, 0),  "kne_L": (6, 0, 0),   "ank_L": (-2, 0, 0),
    "hip_R": (-8, 0, 0),  "kne_R": (50, 0, 0),  "ank_R": (-14, 0, 0),
    "clav_L": (0, 0, 5),  "elb_L": (15, 0, 0),
    "clav_R": (0, 0, -5), "elb_R": (15, 0, 0),
    "spine": (3, 0, 0), "chest": (3, 0, 0),
    "root": (0, 0, 2),
}

# ── the desk ────────────────────────────────────────────────────────────────
# Seated at the console: same leg bend as SIT (he is sitting, the surface
# just happens to be a chair rather than the slab's end), arms brought
# forward and down onto a desk instead of resting at his sides.
SIT_DESK = {
    "hip_R": (-88, 0, 0), "hip_L": (-88, 0, 0),
    "kne_R": (84, 0, 0), "kne_L": (84, 0, 0),
    "ank_R": (-8, 0, 0), "ank_L": (-8, 0, 0),
    "spine": (4, 0, 0), "chest": (6, 0, 0),
    "neck": (5, 0, 0), "head": (8, 0, 0),
    "clav_R": (-70, 0, -8), "clav_L": (-70, 0, 8),
    "elb_R": (95, 0, 0), "elb_L": (95, 0, 0),
}
# Anticipation for sit_down: the knees unlock and the torso comes forward
# before anything descends. Without it he sinks from standing like a lift.
SIT_ANTIC = {
    "hip_R": (-14, 0, 0), "hip_L": (-14, 0, 0),
    "kne_R": (16, 0, 0), "kne_L": (16, 0, 0),
    "ank_R": (-4, 0, 0), "ank_L": (-4, 0, 0),
    "spine": (7, 0, 0), "chest": (6, 0, 0),
    "neck": (2, 0, 0), "head": (4, 0, 0),
    "clav_R": (-12, 0, -8), "clav_L": (-12, 0, 8),
    "elb_R": (22, 0, 0), "elb_L": (22, 0, 0),
}
# Landing: a little past the final pose, compressed, arms trailing. The clip
# then settles back UP to SIT_DESK, which is what sells the chair taking his
# weight rather than him arriving at a height and stopping.
SIT_DEEP = dict(
    SIT_DESK,
    hip_R=(-94, 0, 0), hip_L=(-94, 0, 0),
    kne_R=(90, 0, 0), kne_L=(90, 0, 0),
    spine=(9, 0, 0), chest=(11, 0, 0),
    neck=(7, 0, 0), head=(12, 0, 0),
    clav_R=(-58, 0, -8), clav_L=(-58, 0, 8),
    elb_R=(86, 0, 0), elb_L=(86, 0, 0),
)
# Standing up is a weight transfer before it is a lift: the torso folds
# forward over the feet, THEN the legs extend. Rising straight out of
# SIT_DESK is the move a chair would tip over backwards for.
STAND_ANTIC = dict(
    SIT_DESK,
    spine=(17, 0, 0), chest=(15, 0, 0),
    neck=(-3, 0, 0), head=(-1, 0, 0),
    clav_R=(-80, 0, -6), clav_L=(-80, 0, 6),
    elb_R=(99, 0, 0), elb_L=(99, 0, 0),
)
STAND_RISE = {
    "hip_R": (-12, 0, 0), "hip_L": (-12, 0, 0),
    "kne_R": (14, 0, 0), "kne_L": (14, 0, 0),
    "ank_R": (-3, 0, 0), "ank_L": (-3, 0, 0),
    "spine": (6, 0, 0), "chest": (5, 0, 0),
    "neck": (1, 0, 0), "head": (3, 0, 0),
    "clav_R": (-18, 0, -8), "clav_L": (-18, 0, 8),
    "elb_R": (28, 0, 0), "elb_L": (28, 0, 0),
}

# Typing, as two hands that alternate rather than one gesture applied to
# both. The previous version moved the wrists +10 and -10 — equal and
# OPPOSITE, which bends one hand down onto the keys and the other one up off
# them, and reads as a wave. Every other paired joint in this file (hips,
# knees, ankles) uses the SAME sign on both sides for the same motion, and so
# do these now; the two hands are separated in TIME instead, which is what
# typing actually looks like.
TYPE_R = dict(SIT_DESK, wri_R=(14, 0, 0), wri_L=(2, 0, 0), head=(10, 0, 0))
TYPE_L = dict(SIT_DESK, wri_R=(2, 0, 0), wri_L=(14, 0, 0), head=(7, 0, 0))
TYPE_R2 = dict(SIT_DESK, wri_R=(12, 0, 0), wri_L=(3, 0, 0), head=(9, 0, 0))


# ── clips ───────────────────────────────────────────────────────────────────
# (name, loop, keyframes) — keyframes: [(frame, pose, ease_into_this_pose)].
# The easing on a key describes how the motion ARRIVES at it; the first key of
# a clip has nothing before it, so its slot is None. Lengths are fixed; see
# "What must not move" in the file comment.
SOURCE_CLIPS = [
    # Rise into the held pose quickly and settle ("out"), breathe once at the
    # top, then let the hands down slowly. Frame 0 and frame 108 are both
    # STAND so the loop closes.
    ("idle_distraught", True, [
        (0, STAND, None),
        (34, DISTRAUGHT, "out"),
        (58, DISTRAUGHT_BREATH, "inout"),
        (78, DISTRAUGHT, "inout"),
        (108, STAND, "inout"),
    ]),
    # contact -> pass -> contact -> pass -> contact. Four poses per stride
    # instead of two, sampled every STEP frames: nine keys per bone over 40
    # frames, the density the machine centaur's own walk ships at.
    ("walk", True, [
        (0, WALK_A, None),
        (10, PASS_A, "inout"),
        (20, WALK_B, "inout"),
        (30, PASS_B, "inout"),
        (40, WALK_A, "inout"),
    ]),
    ("sit_down", False, [
        (0, STAND, None),
        (10, SIT_ANTIC, "out"),
        (34, SIT_DEEP, "in"),      # accelerating downward: this is gravity
        (45, SIT_DESK, "out"),     # ...and this is the chair stopping it
    ]),
    ("sit_type", True, [
        (0, SIT_DESK, None),
        (15, TYPE_R, "out"),
        (30, TYPE_L, "out"),
        (45, TYPE_R2, "out"),
        (60, SIT_DESK, "out"),
    ]),
    ("stand_up", False, [
        (0, SIT_DESK, None),
        (12, STAND_ANTIC, "inout"),
        (36, STAND_RISE, "out"),
        (45, STAND, "inout"),
    ]),
    # Re-timed SIT/RECLINE/LIE: the original POSE_KEYS' 3.00s ("turns his
    # back on it") through 8.40s ("head touches down") span, shifted so this
    # clip starts at frame 0. Root translation/pitch for the same span is
    # pm_intake.c's job (KilnTransform), exactly as it always was — this clip
    # carries only the joint bends.
    #
    # SIT is still reached at 84 and RECLINE at 264 and LIE is still last, all
    # unchanged. What is new is SIT_BOW in the middle: frames 84..192 used to
    # be SIT held against itself, 108 frames — nearly two seconds — of a man
    # who is about to do something irreversible sitting perfectly motionless
    # on the slab. The camera is on him for all of it.
    ("climb_in", False, [
        (0, STAND, None),
        (84, SIT, "out"),
        (132, SIT_BOW, "inout"),
        (192, SIT, "inout"),
        (264, RECLINE, "inout"),
        (324, LIE, "out"),
    ]),
]

CLIPS = [(name, loop, expand(keys)) for name, loop, keys in SOURCE_CLIPS]
