#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""ph_anim_clips.py — named animation clips for Dr. Horner's rig.

Pure choreography, no export mechanics: a list of (frame, pose) keyframes per
clip, in ph_rig.py's joint-angle convention (degrees, corrective angles away
from the slumped bind pose — zero is the standing character exactly as
delivered, not a T-pose). `ph_rig_export.py` turns this into the per-bone
keyframe tracks a glTF action needs; nothing here touches Blender or JSON.

Frames are 1:1 with seconds*60 — the importer resamples every action to a
fixed 60 Hz regardless of how it was authored (m64lib.make_action's own
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
"""

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

# A plain walk cycle: opposite hip/knee swing, contralateral arm swing, a
# little torso counter-rotation. WALK_B is WALK_A's mirror, so the cycle
# A -> B -> A (which is what the two-key loop below produces) is one full
# stride, left and right.
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
# Only the wrists move — everything else holds SIT_DESK's pose exactly, so
# sit_type never drifts away from what sit_down/stand_up were keyed against.
TYPE_B = dict(SIT_DESK, wri_R=(10, 0, 0), wri_L=(-10, 0, 0))

# (name, loop, keyframes) — keyframes: [(frame, pose), ...].
CLIPS = [
    ("idle_distraught", True, [
        (0, STAND), (54, DISTRAUGHT), (108, STAND),
    ]),
    ("walk", True, [
        (0, WALK_A), (20, WALK_B), (40, WALK_A),
    ]),
    ("sit_down", False, [
        (0, STAND), (45, SIT_DESK),
    ]),
    ("sit_type", True, [
        (0, SIT_DESK), (30, TYPE_B), (60, SIT_DESK),
    ]),
    ("stand_up", False, [
        (0, SIT_DESK), (45, STAND),
    ]),
    # Re-timed SIT/RECLINE/LIE: the original POSE_KEYS' 3.00s ("turns his
    # back on it") through 8.40s ("head touches down") span, shifted so this
    # clip starts at frame 0. Root translation/pitch for the same span is
    # pm_intake.c's job (M64Transform), exactly as it always was — this clip
    # carries only the joint bends.
    ("climb_in", False, [
        (0, STAND), (84, SIT), (192, SIT), (264, RECLINE), (324, LIE),
    ]),
]
