# SPDX-License-Identifier: MIT
"""goblin.py — the four playable goblins, rigged, animated, and able to ride.

    blender --background --factory-startup -noaudio --python goblin.py \
        -- --model dank --out build/dank

    --model dank | sparky | moss | glimmer | goblin

A downstream party game (now in its own repo) has four playable characters,
each carrying its own passive and special via kiln_char.h. This file is all
four, plus `goblin`, the neutral build the earlier demos loaded and the one
to look at when you want the shape language without a character on top of
it.

── One rig, four bodies ───────────────────────────────────────────────────
Every character uses the SAME skeleton: identical bone names, identical rest
head/tail positions, no exceptions. Their proportions differ entirely in the
geometry hung off those bones — Dank's belly is a wider mass on the same
torso bone, Sparky's limbs are thinner prisms between the same joints.

That is a deliberate constraint and it buys the thing a game actually
needs: one set of animations that plays on all four. A game picks a
character per player at runtime (a character-select screen) and then
runs the same Idle/Walk/Ride actions whichever was picked. If Sparky's rig
were genuinely taller, every action would need a Sparky variant, the ROM
would carry four copies of the animation data, and the four would drift
apart the first time one was tweaked.

The cost is that silhouette differences have to be made with mass, not with
bone length — which is why Sparky reads as tall and thin rather than being
tall, and why Moss is squat by having short thick limb geometry on the same
leg bones. At this poly count that trade is invisible and heavily in favour
of the shared rig.

── Rigid segments, not smooth weights ─────────────────────────────────────
The importer allows exactly ONE bone per vertex, and at most three bones per
triangle (Tiny3D README). Blender's "with automatic weights" parenting
produces neither — it spreads each vertex across several bones, and the
importer keeps whichever it saw first, so the mesh tears at every joint in a
way that looks like a corrupt export rather than a rigging mistake.

So every piece is a separate rigid chunk welded to exactly one bone. The
articulated-puppet look that forces is leaned into rather than fought.

── Authored high, welded down ─────────────────────────────────────────────
Tiny3D stores a vertex position as int16 — the integer part of an s16.16 —
so at the default --base-scale=64 the console's vertex grid is exactly 1/64
of a Blender unit. Detail finer than that is not merely lost at conversion,
it is destructive: two vertices a two-hundredth of a unit apart land on the
same integer and the triangle between them becomes a zero-area sliver that
still costs a vertex slot and still costs RSP transform time.

So every part goes through _emit(), which snaps it to that grid and then
merges what the snapping made redundant (kilnlib.weld). Per character:

    Dank    827 -> 512 verts    862 -> 880 tris
    Sparky  851 -> 510          854 -> 872
    Moss    875 -> 510          850 -> 868
    Glimmer 842 -> 502          856 -> 856

The vertex column is the one that matters on this console: the RSP transforms
per vertex and Tiny3D chunks a mesh every 70 of them, so a 40% reclaim is
40% fewer transforms and fewer chunk boundaries, for identical geometry. The
triangle count went UP over the previous version by design — the detail is
the point — but it is buying a rounded skull, six-sided limbs, shaped boots
and eyes with actual pupils, not sub-grid noise.

Welding happens PER PART, and that is load-bearing rather than incidental:
skinning is rigid, one bone per vertex, so two vertices that coincide across
a joint must not merge or the elbow tears the moment the forearm rotates.
Keeping the weld inside a part makes that impossible rather than something
to remember.

── Shaped masses, not stacked boxes ───────────────────────────────────────
The first version of this model was axis-aligned boxes throughout, and it
looked like it: a cube head on a cube chest with cube arms hanging off it,
every silhouette a rectangle whichever way you turned it. Rigid skinning
does not require that. It requires one bone per vertex, which says nothing
about the shape of the chunk.

So the masses are lofted now — hips, chest and skull are tapered octagonal
and hexagonal stacks with a real waist, brow and jawline — and the limbs are
kilnlib.segment() prisms running between the ACTUAL joint positions rather
than upright boxes centred near them. The arm bone runs 37 degrees off
vertical; a vertical box on it covers the bone approximately, and
"approximately" is what made the old model read as unrelated blocks floating
near a skeleton.

── He faces +Y ────────────────────────────────────────────────────────────
He used to face -Y: nose, eyes, jaw and toes all pointed that way. Everything
else authored here — interceptor.py, and a downstream game's own vehicle
models — puts the nose along +Y, which export_yup turns into the -Z that
glTF and Tiny3D call forward. Nothing caught it, because a lone character
has no second object to disagree with; the moment he had to sit in a
vehicle he sat in it backwards.

── The joints exist for riding ────────────────────────────────────────────
The rig has a neck, an elbow (`forearm_*`) and a knee (`shin_*`). A standing
waddle needs none of them. A seated pose does: without a knee the thigh
either reaches the pedals with the foot pointing into the floor, or reaches
the floor and misses the pedals, and no rotation of a single bone gets both.

── Proportions ────────────────────────────────────────────────────────────
~2.3 Blender units tall, standing on Z=0, so at the default --base-scale=64
that is ~150 Tiny3D units — comfortably inside the int16 position range.

The limb chains and a vehicle's own rider station are sized against each
other, and the numbers are tight enough to be worth stating: shoulder-to-grip
is 0.55 against an arm chain of 0.82, hip-to-ankle 0.48 against a leg chain
of 0.52. Both reaches are shorter than their chain, so the riding poses come
out with a natural bend rather than a limb visibly stretched straight to fit
— and the leg has almost no margin, because these are short goblin legs and
a first attempt at a station put the footrests 0.69 away from a 0.52 chain.
A test measuring the actual posed distance on every run is what caught that.
"""

import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import kilnlib as m  # noqa: E402

# ── shared palette entries ─────────────────────────────────────────────────
# Colours every character shares, so four goblins standing together read as
# the same species in different moods rather than as four unrelated props.
EYE_WHITE = m.srgb(248, 246, 214)
PUPIL = m.srgb(24, 20, 18)
TOOTH = m.srgb(238, 232, 200)
BOOT = m.srgb(58, 44, 38)
BOOT_SOLE = m.srgb(36, 28, 24)
NAIL = m.srgb(212, 200, 160)


def _shade(base, factor):
    """A darker or lighter relative of a colour, kept in one place so every
    character's shadow and highlight are derived the same way. Characters
    supply ONE skin colour; the ramp comes from here."""
    r, g, b, a = base
    return (min(1.0, r * factor), min(1.0, g * factor),
            min(1.0, b * factor), a)


# ── the four ───────────────────────────────────────────────────────────────
# Each entry is a shape + palette spec. The builder below reads it; nothing
# here is code, so a fifth goblin is a table entry and no new geometry.
#
#   girth      belly/chest mass multiplier      (1.0 = the neutral build)
#   limb       limb thickness multiplier
#   head       skull size multiplier
#   brow       how far the brow ridge overhangs
#   eye        eye box height (droopy vs wide)
#   ear        "droop" | "spike" | "round" | "long"
#   stoop      forward tilt baked into the hips, in degrees
#   extra      name of the character-specific detail builder, or None
GOBLINS = {
    "goblin": dict(
        label="Goblin", skin=m.srgb(122, 176, 74), tunic=m.srgb(118, 74, 52),
        belt=m.srgb(72, 46, 34), accent=m.srgb(150, 150, 70),
        girth=1.00, limb=1.00, head=1.00, brow=1.00, eye=1.00,
        ear="droop", stoop=0.0, extra=None,
    ),
    # Dank: the mellow one. Heaviest of the four, lowest centre of mass, a
    # permanent slouch and eyes at half mast. Everything about the shape says
    # "not in a hurry", which is what a slow-and-steady passive ability
    # plays like.
    "dank": dict(
        label="Dank", skin=m.srgb(96, 148, 84), tunic=m.srgb(86, 62, 104),
        belt=m.srgb(52, 38, 64), accent=m.srgb(206, 176, 96),
        girth=1.28, limb=1.16, head=1.02, brow=1.15, eye=0.55,
        ear="droop", stoop=7.0, extra="beanie",
    ),
    # Sparky: wiry, jittery, all elbows. Thin limbs and a small head on a
    # narrow chest, goggles pushed up on the brow.
    "sparky": dict(
        label="Sparky", skin=m.srgb(176, 200, 62), tunic=m.srgb(188, 96, 40),
        belt=m.srgb(120, 58, 24), accent=m.srgb(255, 214, 84),
        girth=0.80, limb=0.82, head=0.94, brow=0.80, eye=1.35,
        ear="spike", stoop=-4.0, extra="goggles",
    ),
    # Moss: squat and overgrown, more boulder than goblin. The widest and
    # shortest-limbed, with a heavy brow and clumps growing on him.
    "moss": dict(
        label="Moss", skin=m.srgb(74, 106, 58), tunic=m.srgb(78, 58, 40),
        belt=m.srgb(48, 36, 26), accent=m.srgb(140, 176, 92),
        girth=1.34, limb=1.22, head=1.10, brow=1.35, eye=0.70,
        ear="round", stoop=4.0, extra="moss",
    ),
    # Glimmer: slender and pale, with crystal growing out of her shoulders.
    # The only one of the four whose accent is a cool colour, which is most of
    # why she reads as the odd one out in a line-up.
    "glimmer": dict(
        label="Glimmer", skin=m.srgb(128, 190, 168), tunic=m.srgb(72, 52, 120),
        belt=m.srgb(44, 32, 76), accent=m.srgb(150, 236, 255),
        girth=0.86, limb=0.90, head=0.96, brow=0.70, eye=1.30,
        ear="long", stoop=-2.0, extra="crystal",
    ),
}

# ── skeleton ───────────────────────────────────────────────────────────────
# IDENTICAL for every character — see the file comment. (name, parent, head,
# tail). The armature object itself stays at the origin with no transform:
# gltf_to_t3d throws "At least one ancestor of armature/skin root bone has
# significant transforms!" otherwise, and the fix is always to move the
# geometry, never the armature.
BONES = [
    ("root",      None,        (0.00, 0.00, 0.72), (0.00, 0.00, 0.98)),
    ("torso",     "root",      (0.00, 0.00, 0.98), (0.00, 0.00, 1.40)),
    ("neck",      "torso",     (0.00, 0.00, 1.40), (0.00, 0.00, 1.52)),
    ("head",      "neck",      (0.00, 0.00, 1.52), (0.00, 0.00, 2.10)),
    ("jaw",       "head",      (0.00, 0.10, 1.62), (0.00, 0.34, 1.52)),
    ("nose",      "head",      (0.00, 0.18, 1.80), (0.00, 0.66, 1.66)),
    ("ear_l",     "head",      (0.30, 0.00, 1.88), (0.76, -0.06, 2.20)),
    ("ear_r",     "head",      (-0.30, 0.00, 1.88), (-0.76, -0.06, 2.20)),

    ("arm_l",     "torso",     (0.36, 0.00, 1.34), (0.56, 0.00, 1.08)),
    ("forearm_l", "arm_l",     (0.56, 0.00, 1.08), (0.68, 0.00, 0.84)),
    ("hand_l",    "forearm_l", (0.68, 0.00, 0.84), (0.74, 0.00, 0.62)),
    ("arm_r",     "torso",     (-0.36, 0.00, 1.34), (-0.56, 0.00, 1.08)),
    ("forearm_r", "arm_r",     (-0.56, 0.00, 1.08), (-0.68, 0.00, 0.84)),
    ("hand_r",    "forearm_r", (-0.68, 0.00, 0.84), (-0.74, 0.00, 0.62)),

    ("thigh_l",   "root",      (0.20, 0.00, 0.72), (0.21, 0.00, 0.46)),
    ("shin_l",    "thigh_l",   (0.21, 0.00, 0.46), (0.22, 0.00, 0.20)),
    ("foot_l",    "shin_l",    (0.22, 0.00, 0.20), (0.23, 0.30, 0.06)),
    ("thigh_r",   "root",      (-0.20, 0.00, 0.72), (-0.21, 0.00, 0.46)),
    ("shin_r",    "thigh_r",   (-0.21, 0.00, 0.46), (-0.22, 0.00, 0.20)),
    ("foot_r",    "shin_r",    (-0.22, 0.00, 0.20), (-0.23, 0.30, 0.06)),
]

# ── cross-sections ─────────────────────────────────────────────────────────
# Authored counter-clockwise seen from +Z, which _stack() then REVERSES.
#
# loft() winds correctly when each ring runs in increasing angle about the
# sweep axis in the sense that makes U x V = -T (see sweep()'s docstring for
# the derivation). For a +Y sweep (a vehicle body's long axis, say) that
# works out to increasing atan2(z, x); for a +Z sweep it is the other way
# round —
# CLOCKWISE seen from above. Authoring anticlockwise and reversing once, in
# one place, is less error-prone than remembering which way round a given
# axis wants, and test_goblins.py catches it either way.
def _poly(n):
    """An n-sided unit profile, anticlockwise. Higher n is a rounder mass;
    the weld pass reclaims whatever the console's 1/64 grid cannot keep."""
    return [(math.cos(2.0 * math.pi * i / n), math.sin(2.0 * math.pi * i / n))
            for i in range(n)]


HEX = _poly(6)
OCT = _poly(8)
DEC = _poly(10)      # the skull, the roundest mass on the model


def _stack(profile, stations):
    """Loft a profile up +Z. stations: [(z, half_x, half_y, centre_y), ...].

    Stations must run in increasing z. The profile is reversed here, once —
    see the comment on the profiles above.
    """
    rings = []
    for z, sx, sy, cy in stations:
        rings.append([(px * sx, cy + py * sy, z) for px, py in reversed(profile)])
    return m.loft(rings)


def _along_y(profile, stations):
    """Loft a profile along +Y. stations: [(y, half_x, half_z, centre_z), ...].

    The +Y counterpart of _stack, and it does NOT reverse the profile — a
    +Y sweep wants increasing atan2(z, x), which is the order _poly already
    produces when its two components are read as (x, z). Two sweep axes, two
    opposite conventions; that is loft()'s rule, not a choice made here.
    """
    rings = []
    for y, sx, sz, cz in stations:
        rings.append([(px * sx, y, cz + pz * sz) for px, pz in profile])
    return m.loft(rings)


# Accumulated across every part of one character, printed by main(). The
# point of reporting it is that "author high, weld down" is only worth doing
# if the reclaim is real — so the number is measured, never assumed.
WELD = {"verts_in": 0, "verts_out": 0, "tris_in": 0, "tris_out": 0,
        "dropped": 0}


def _emit(parts, bone, verts, faces, colors):
    """Weld a part to the console's vertex grid and add it to the mesh.

    EVERY piece of geometry goes through here. Welding per part rather than
    over the finished mesh is deliberate and load-bearing: skinning is rigid,
    one bone per vertex, so two vertices that happen to coincide across a
    joint MUST NOT merge — the elbow would tear the moment the forearm
    rotated. Keeping the weld inside a part makes that impossible to get
    wrong rather than something to remember.
    """
    v, f, c, st = m.weld(verts, faces, colors)
    for k in WELD:
        WELD[k] += st[k]
    parts.append((bone, v, f, c))


def _box(cx, cy, cz, sx, sy, sz, color):
    """A box as the (verts, faces, colors) triple the parts list wants.

    `color` is one srgb() or a list of six, one per face. The six-entry form
    works because kilnlib.box() splits all 24 corners per face, so nothing is
    averaged — which is how the eyes get their pupils for free, as a darkened
    front face rather than extra geometry.
    """
    verts, faces = m.box(cx, cy, cz, sx, sy, sz)
    return verts, faces, m.expand_colors("box", color, verts, faces)


# Face indices into the six-colour form of _box. kilnlib.box() returns faces
# in the order -Z, +Z, -Y, +X, +Y, -X, so FRONT is index 4 now that he faces
# +Y (it used to be index 2, which is exactly the sort of thing that breaks
# silently when a model is turned around).
F_BOT, F_TOP, F_BACK, F_RIGHT, F_FRONT, F_LEFT = range(6)


def _ramp(verts, lo, hi, c_lo, c_hi, axis=2):
    """Per-vertex colours from a linear ramp along one axis."""
    span = (hi - lo) or 1.0
    return [m.mix(c_lo, c_hi, max(0.0, min(1.0, (v[axis] - lo) / span)))
            for v in verts]


# ── ears ───────────────────────────────────────────────────────────────────
# Real thin wedges, not zero-thickness fins. The first version was two
# coincident triangles wound opposite ways, which is a plausible-looking way
# to get a double-sided surface out of a back-face-culling renderer and a bad
# one: the two faces are coplanar, so the RDP's Z-buffer decides between them
# per pixel and the ear shimmers along its whole length as the camera moves.
#
# Shape is per character and it is the cheapest silhouette difference in the
# model — an ear reads from further away than a face does.
EAR_PLATES = {
    "droop": [(0.00, -0.16, 0.0, 0.034), (0.34, -0.16, 0.0, 0.026),
              (0.44, 0.04, 0.0, 0.018), (0.05, 0.18, 0.0, 0.034)],
    "spike": [(0.00, -0.14, 0.0, 0.034), (0.40, 0.10, 0.0, 0.024),
              (0.62, 0.52, 0.0, 0.014), (0.06, 0.20, 0.0, 0.034)],
    "round": [(0.00, -0.14, 0.0, 0.036), (0.24, -0.10, 0.0, 0.032),
              (0.28, 0.12, 0.0, 0.026), (0.03, 0.17, 0.0, 0.036)],
    "long":  [(0.00, -0.12, 0.0, 0.030), (0.52, 0.04, 0.0, 0.020),
              (0.86, 0.40, 0.0, 0.010), (0.06, 0.16, 0.0, 0.030)],
}


def _ear(spec, x_sign):
    """One ear: authored flat in XY (x = outward span, y = height) because
    slab() extrudes along Z, then stood upright by a proper rotation — the
    same rotate-don't-reflect discipline a mirrored wheel or panel wants."""
    plate = EAR_PLATES[spec["ear"]]
    if x_sign < 0:
        plate = m.mirror_x(plate)
    verts, faces = m.slab(plate)
    verts = m.rotated(verts, (1, 0, 0), 90)          # height -> +Z
    verts = m.translated(verts, dx=0.28 * x_sign, dy=0.02, dz=1.84)

    skin = spec["skin"]
    tip = max(abs(p[0]) for p in plate) or 1.0
    colors = [m.mix(skin, _shade(skin, 0.62),
                    min(1.0, abs(v[0] - 0.28 * x_sign) / tip)) for v in verts]
    return verts, faces, colors


# ── character extras ───────────────────────────────────────────────────────

def _extra_beanie(spec, parts):
    """Dank's slouchy hat: two lofted rings and a bobble. The hat brim sits
    lower at the back than the front, which is what makes it read as pulled
    on rather than as a cylinder resting on his head."""
    wool = spec["tunic"]
    verts, faces = _stack(HEX, [
        (2.02, 0.42, 0.36, 0.01),
        (2.18, 0.40, 0.34, -0.01),
        (2.28, 0.28, 0.24, -0.04),
    ])
    _emit(parts, "head", verts, faces,
          _ramp(verts, 2.02, 2.28, _shade(wool, 0.7), wool))
    verts, faces = m.cylinder(0.10, 0.12, 6)
    verts = m.translated(verts, dy=-0.05, dz=2.26)
    _emit(parts, "head", verts, faces, [spec["accent"]] * len(verts))


def _extra_goggles(spec, parts):
    """Sparky's goggles, pushed up onto the brow. Two lens drums and a strap
    band; the lenses face +Y so their bright caps catch the eye from the
    front, the same trick the motorcycle's headlight uses."""
    strap = _shade(spec["tunic"], 0.65)
    verts, faces = m.box(0.0, 0.16, 2.08, 0.76, 0.20, 0.12)
    _emit(parts, "head", verts, faces, [strap] * len(verts))
    for sx in (1, -1):
        verts, faces = m.cylinder(0.15, 0.16, 5, cz=0.0)
        verts = m.rotated(verts, (1, 0, 0), -90)
        verts = m.translated(verts, dx=0.20 * sx, dy=0.20, dz=2.08)
        lens_y = max(v[1] for v in verts)
        colors = [spec["accent"] if v[1] > lens_y - 1e-4 else strap
                  for v in verts]
        _emit(parts, "head", verts, faces, colors)


def _extra_moss(spec, parts):
    """Moss's namesake: clumps on the crown and both shoulders. Deliberately
    NOT symmetric — a matched pair reads as pauldrons, and the whole point is
    that the stuff grew there."""
    clump = spec["accent"]
    for cx, cy, cz, sx, sy, sz, bone in (
        (0.10, -0.06, 2.12, 0.34, 0.30, 0.14, "head"),
        (-0.18, 0.08, 2.06, 0.22, 0.22, 0.10, "head"),
        (0.34, 0.02, 1.40, 0.30, 0.28, 0.14, "torso"),
        (-0.30, -0.04, 1.36, 0.22, 0.24, 0.11, "torso"),
    ):
        verts, faces = m.box(cx, cy, cz, sx, sy, sz)
        _emit(parts, bone, verts, faces,
              _ramp(verts, cz - sz / 2, cz + sz / 2,
                    _shade(clump, 0.55), clump))


def _extra_crystal(spec, parts):
    """Glimmer's shards: cones growing out of her shoulders and crown. Cones
    rather than boxes because the taper is the whole read at this size, and a
    5-sided cone is 10 tris."""
    tip = spec["accent"]
    base = _shade(tip, 0.45)
    for x, y, z, r, h, lean, bone in (
        (0.30, -0.10, 1.42, 0.10, 0.36, 22, "torso"),
        (-0.26, -0.12, 1.38, 0.08, 0.28, -18, "torso"),
        (0.06, -0.10, 2.10, 0.09, 0.30, 8, "head"),
    ):
        verts, faces = m.cylinder(r, h, 6, top_radius=0.0)
        verts = m.rotated(verts, (0, 1, 0), lean)
        verts = m.translated(verts, dx=x, dy=y, dz=z)
        _emit(parts, bone, verts, faces, _ramp(verts, z, z + h, base, tip))


EXTRAS = {
    "beanie": _extra_beanie,
    "goggles": _extra_goggles,
    "moss": _extra_moss,
    "crystal": _extra_crystal,
}


# ── the body ───────────────────────────────────────────────────────────────

def build_goblin(spec):
    armature = m.make_armature("GoblinRig", BONES)

    skin = spec["skin"]
    skin_dark = _shade(skin, 0.56)
    skin_lit = _shade(skin, 1.34)
    belly = _shade(skin, 1.22)
    tunic = spec["tunic"]
    limb = spec["limb"]
    hs = spec["head"]

    # ── girth goes FORWARD, mostly ─────────────────────────────────────
    # Applying `girth` equally to width and depth is the obvious reading and
    # it is wrong. The shoulder joints are fixed at x = +/-0.36 by the shared
    # rig; at Dank's 1.28 an evenly-scaled chest is 0.95 across, so both arms
    # START INSIDE THE TORSO and only emerge at the elbow. Rendered, that is
    # not "a heavy goblin", it is a green lump with hands.
    #
    # So width takes about a third of the girth and depth takes all of it.
    # A pot belly sticks out in front of you, which is both anatomically the
    # point and the direction with no joint in the way.
    gw = 1.0 + (spec["girth"] - 1.0) * 0.35
    gd = spec["girth"]
    g = gw   # anything still measured across the body

    parts = []

    # ── hips ───────────────────────────────────────────────────────────
    # A tapered stack rather than a box, so there is a waist for the belt to
    # sit in. The stoop is baked in here as a per-station Y offset, which
    # tilts the whole upper body without touching the rest pose — a rotated
    # rest pose would make every animation start from somewhere the action
    # curves do not know about.
    st = spec["stoop"] * 0.004
    verts, faces = _stack(OCT, [
        (0.64, 0.25 * gw, 0.21 * gd, 0.00),
        (0.76, 0.29 * gw, 0.25 * gd, st * 0.6),
        (0.88, 0.32 * gw, 0.27 * gd, st * 1.2),
        (1.00, 0.29 * gw, 0.25 * gd, st * 2.0),
    ])
    _emit(parts, "root", verts, faces,
          _ramp(verts, 0.64, 1.00, _shade(tunic, 0.62), tunic))
    verts, faces = _stack(OCT, [
        (0.96, 0.315 * gw, 0.285 * gd, st * 1.9),
        (1.05, 0.315 * gw, 0.285 * gd, st * 2.2),
    ])
    _emit(parts, "root", verts, faces,
          _ramp(verts, 0.96, 1.05, _shade(spec["belt"], 0.7), spec["belt"]))

    # ── chest and belly ────────────────────────────────────────────────
    # Widest low, which is the pot belly, narrowing to the shoulders. Four
    # stations: the extra one at the belly's widest point is what stops the
    # taper reading as a straight cone.
    verts, faces = _stack(OCT, [
        (1.02, 0.27 * gw, 0.23 * gd, st * 2.2),
        (1.12, 0.30 * gw, 0.28 * gd, st * 2.6),
        (1.22, 0.31 * gw, 0.30 * gd, st * 3.2),
        (1.34, 0.32 * gw, 0.26 * gd, st * 4.0),
        (1.44, 0.30 * gw, 0.23 * gd, st * 4.6),
        (1.52, 0.24 * gw, 0.19 * gd, st * 5.0),
    ])
    # ── the single most important colour decision in the model ─────────
    # He wears a tunic on his CHEST and the belly hangs out underneath it.
    #
    # Before this the hips, belly, chest, arms, jaw and head were all the
    # same green, and the render showed exactly what that gets you: a lumpy
    # green mass with facets catching the light at random and no way to tell
    # where the body ended and the arm began. Adding geometry does not fix
    # that — the shapes were already there, they just had no value break
    # between them.
    #
    # So the upper stations take the tunic colour and the lower ones stay
    # bare skin. It costs nothing (same vertices, different COLOR_0), it puts
    # a hard horizontal value break across the middle of the silhouette, and
    # the bare band between tunic hem and belt reads as a pot belly under a
    # tunic that no longer fits — which is the right joke for these four.
    colors = []
    for v in verts:
        if v[2] >= 1.30:
            c = m.mix(_shade(tunic, 0.70), tunic,
                      max(0.0, min(1.0, (v[2] - 1.30) / 0.20)))
        else:
            c = m.mix(skin_dark, skin,
                      max(0.0, min(1.0, (v[2] - 1.00) / 0.20)))
            # Front faces paler — a belly, not a back.
            front = max(0.0, min(1.0, (v[1] - 0.02) / (0.30 * gd)))
            c = m.mix(c, belly, front * 0.85)
        colors.append(c)
    _emit(parts, "torso", verts, faces, colors)

    verts, faces = _stack(HEX, [
        (1.40, 0.15, 0.14, st * 5.2),
        (1.52, 0.17, 0.16, st * 5.4),
    ])
    _emit(parts, "neck", verts, faces, [skin_dark] * len(verts))

    # ── skull ──────────────────────────────────────────────────────────
    # Deliberately out of proportion: about as wide as the chest and nearly
    # as tall. Most of the character reads from this one mass, so it gets an
    # octagonal section and five stations — narrow at the neck, widest at the
    # cheekbones, drawn back in over the crown.
    verts, faces = _stack(DEC, [
        (1.50, 0.22 * hs, 0.20 * hs, 0.00),
        (1.62, 0.38 * hs, 0.33 * hs, 0.01),
        (1.78, 0.45 * hs, 0.38 * hs, 0.01),
        (1.96, 0.45 * hs, 0.38 * hs, 0.00),
        (2.09, 0.37 * hs, 0.31 * hs, -0.02),
        (2.18, 0.22 * hs, 0.18 * hs, -0.04),
    ])
    colors = []
    for v in verts:
        c = m.mix(skin_dark, skin, max(0.0, min(1.0, (v[2] - 1.50) / 0.34)))
        colors.append(m.mix(c, skin_lit,
                            max(0.0, min(1.0, (v[2] - 1.96) / 0.22))))
    _emit(parts, "head", verts, faces, colors)

    # A heavy brow. One box, and it does more for the face than the eyes do:
    # without it the eye blocks sit on a flat wall and he reads as surprised
    # rather than as permanently unimpressed.
    bw = spec["brow"]
    _emit(parts, "head", *_box(0, 0.30 * hs + 0.02 * bw, 1.985,
                               0.58 * hs, 0.09 * bw, 0.075 * bw,
                               _shade(skin, 0.5)))

    # Eyes: an eyeball box with the front face darkened into a pupil, plus a
    # separate LID wedge over the top of it.
    #
    # The lid is the detail that buys the most expression per triangle on the
    # whole model. Without it the eye is a rectangle of white and every
    # character stares; with it, how far the lid comes down IS the
    # expression, and `eye` in the character table drives both the eyeball
    # height and the lid drop from one number. Dank ends up permanently half
    # asleep, Sparky permanently startled, and neither needed new geometry —
    # only different values in the same two boxes.
    eh = spec["eye"]
    lid_drop = 0.10 * (1.35 - eh)
    for sx in (1, -1):
        # The eyeball is white on EVERY face and the pupil is its own small
        # box in front of it. Darkening the eyeball's front face instead —
        # which is what this did, and it is the obvious saving — makes the
        # whole visible area of the eye one flat dark rectangle, because the
        # front face is the only one a player ever sees. Every character
        # ended up with two black slots for eyes and no gaze direction at
        # all. 12 tris each is the cheapest expression in the model.
        _emit(parts, "head", *_box(0.18 * hs * sx, 0.30 * hs, 1.87,
                                   0.20 * hs, 0.10, 0.22 * eh,
                                   [EYE_WHITE] * 6))
        _emit(parts, "head", *_box(0.185 * hs * sx, 0.345 * hs, 1.868,
                                   0.105 * hs, 0.03, 0.115 * eh + 0.02,
                                   [PUPIL] * 6))
        eye_top = 1.87 + 0.11 * eh
        lid_h = 0.06 + lid_drop
        lv, lf = m.box(0.18 * hs * sx, 0.315 * hs, eye_top - lid_h * 0.5,
                       0.22 * hs, 0.11, lid_h)
        lv = m.rotated(lv, (1, 0, 0), -8,
                       origin=(0.18 * hs * sx, 0.315 * hs, eye_top))
        _emit(parts, "head", lv, lf, [_shade(skin, 0.78)] * len(lv))

    # A big hooked nose: a cone laid along +Y and drooping.
    #
    # Laid over by a ROTATION. The version this replaces remapped the cone's
    # coordinates by hand as (x, y, z) -> (x, z, y*0.8 - ...), which swaps two
    # axes — a reflection, determinant -1, so every face came out wound
    # inwards and the nose rendered as a hole in his face from outside and a
    # solid cone from within. It survived a long time because a back-facing
    # cone against a green head at 320x240 just looks like a slightly odd
    # nose. test_goblins.py's signed-volume check is what finally named it.
    cone_v, cone_f = m.cylinder(0.16 * hs, 0.56, 6, top_radius=0.0)
    cone_v = m.rotated(cone_v, (1, 0, 0), -104)      # -90 is level; -104 droops
    cone_v = m.translated(cone_v, dy=0.14, dz=1.82)
    _emit(parts, "nose", cone_v, cone_f,
          _ramp(cone_v, 1.60, 1.86, _shade(spec["accent"], 0.62),
                spec["accent"]))

    _emit(parts, "ear_l", *_ear(spec, 1))
    _emit(parts, "ear_r", *_ear(spec, -1))

    # Jaw with two crooked tusks that stick out past the lip.
    verts, faces = _stack(HEX, [
        (1.45, 0.16 * hs, 0.11, 0.24),
        (1.53, 0.20 * hs, 0.13, 0.23),
        (1.61, 0.19 * hs, 0.12, 0.20),
    ])
    _emit(parts, "jaw", verts, faces,
          _ramp(verts, 1.45, 1.61, _shade(skin, 0.38), _shade(skin, 0.52)))
    for sx, h, cant in ((1, 0.19, 11), (-1, 0.14, -7)):
        tv, tf = m.box(0.13 * hs * sx, 0.31, 1.58 + h * 0.35,
                       0.065, 0.065, h)
        tv = m.rotated(tv, (0, 1, 0), cant,
                       origin=(0.13 * hs * sx, 0.31, 1.58))
        _emit(parts, "jaw", tv, tf, [TOOTH] * len(tv))

    # ── limbs ──────────────────────────────────────────────────────────
    # segment() runs each prism between the ACTUAL joint positions, so the
    # geometry follows the bone instead of approximating it with an upright
    # box. Tapered, thicker at the proximal end, twisted 45 degrees so a flat
    # face rather than an edge points at the camera.
    for side, sx in (("l", 1), ("r", -1)):
        # The prism starts INBOARD of the shoulder joint so it sinks into the
        # chest instead of butting up against it. Starting exactly on the
        # joint leaves a visible slot of background between arm and body at
        # every angle — interpenetration is the technique here, same as the
        # vehicles, and it costs nothing on a Z-buffered renderer.
        shoulder = (0.30 * sx, 0, 1.37)
        elbow = (0.56 * sx, 0, 1.08)
        wrist = (0.68 * sx, 0, 0.84)
        palm = (0.76 * sx, 0, 0.60)

        # Six-sided rather than four: at 4 a limb is a square rod and every
        # angle shows a hard edge down its length. 6 costs 8 tris a limb and
        # is the difference between a rod and an arm.
        v, f = m.segment(shoulder, elbow, 0.155 * limb, 0.12 * limb, 5, 36)
        _emit(parts, f"arm_{side}", v, f,
              _ramp(v, 1.08, 1.37, skin_dark, skin))
        v, f = m.segment(elbow, wrist, 0.12 * limb, 0.098 * limb, 5, 36)
        _emit(parts, f"forearm_{side}", v, f,
              _ramp(v, 0.84, 1.08, skin_dark, skin))

        # Hands stay BOXY: they are the one part that has to read as
        # square-on to a grip, and a round prism reads as a stump. But a bare
        # box is a mitten, so the palm gets a knuckle ridge and a thumb.
        _emit(parts, f"hand_{side}",
              *_box(0.72 * sx, 0.01, 0.74, 0.24 * limb, 0.22, 0.24, skin))
        _emit(parts, f"hand_{side}",
              *_box(0.72 * sx, 0.13, 0.70, 0.22 * limb, 0.08, 0.19,
                    _shade(skin, 0.74)))
        _emit(parts, f"hand_{side}",
              *_box(0.60 * sx, 0.09, 0.77, 0.10, 0.14, 0.13, skin))

        hip = (0.20 * sx, 0, 0.76)
        knee = (0.21 * sx, 0, 0.46)
        ankle = (0.22 * sx, 0, 0.20)
        v, f = m.segment(hip, knee, 0.195 * limb, 0.155 * limb, 5, 36)
        _emit(parts, f"thigh_{side}", v, f,
              _ramp(v, 0.46, 0.76, _shade(skin, 0.6), skin_dark))
        v, f = m.segment(knee, ankle, 0.16 * limb, 0.125 * limb, 5, 36)
        _emit(parts, f"shin_{side}", v, f,
              _ramp(v, 0.20, 0.46, _shade(skin, 0.6), skin_dark))

        # Boots: lofted along +Y with a raised heel and a turned-up toe,
        # rather than the flat box they were. The toe is what makes a big
        # boot read as a boot instead of as a brick.
        bl = 0.30 * limb
        v, f = _along_y(HEX, [
            (-0.18, 0.13 * bl / 0.30, 0.11, 0.13),
            (0.08, 0.15 * bl / 0.30, 0.11, 0.11),
            (0.32, 0.11 * bl / 0.30, 0.07, 0.11),
        ])
        _emit(parts, f"foot_{side}", m.translated(v, dx=0.22 * sx), f,
              _ramp(v, 0.02, 0.22, BOOT_SOLE, BOOT))

    if spec["extra"]:
        EXTRAS[spec["extra"]](spec, parts)

    m.make_skinned_mesh("Goblin", parts, armature, "GoblinMat")
    return armature

# ── animations ─────────────────────────────────────────────────────────────
# Rotations in degrees. Every looping action returns to its frame-0 pose at
# its last frame, so it cycles without a visible snap.
#
# ── Sign conventions, established by MEASURING ─────────────────────────────
# Not by reasoning about Blender's bone axes. A first attempt at this put
# the goblin's hands 1.25 units behind the grips, which is what a wrong guess
# about this looks like — caught by a test that poses the rig and measures.
#
#   X   For LIMB bones (which rest pointing DOWN) positive swings FORWARD
#       (+Y). For SPINE bones (which rest pointing UP) the same rotation
#       swings BACKWARD. Same number, opposite result, because the bones
#       point opposite ways.
#
#   Z   Swings laterally, and the left and right limbs have MIRRORED local
#       frames. An OUTWARD raise is therefore NEGATIVE Z on the left limb and
#       POSITIVE Z on the right. A symmetric pose is naturally written with
#       opposite signs — but picking the wrong one of the two folds the limb
#       ACROSS the body instead of spreading it, and at this poly count that
#       reads as "arms crossed", not as "wrong sign".
#
#       Wave and Taunt had it backwards from the day they were written, and
#       it survived because a goblin with an arm folded across his chest
#       still looks like he is doing something. Found with a probe that
#       rotates one bone and prints the world position of its tip; that is
#       the only way to settle this, and guessing has now cost two rounds.
#
#   Y   Twists. The ears use it; nothing else does.
#
# ── Why every action is baked, not keyed by hand ───────────────────────────
# make_action forces LINEAR interpolation on every F-curve, and that is the
# right call for the reason documented there. It also means a hand-keyed
# action moves at a constant speed between its poses, which is precisely the
# thing that reads as "programmer animation" — no weight, no snap, no settle.
#
# So the poses below are keyframes in the animator's sense, and _bake()
# turns them into the dense linear keys the exporter wants, applying:
#
#   * EASING, sampled from kilnlib.ease() — the curve lives in the keys
#     because it cannot live in the interpolation.
#   * OVERSHOOT, via the "over" mode: a limb travels past its target and
#     settles back. This is most of what makes a move feel like it has mass.
#   * FOLLOW-THROUGH / DRAG, via LAG: floppy parts sample the timeline a few
#     frames in the PAST, so ears and nose arrive after the head that threw
#     them. Left and right lag by different amounts on purpose — matched
#     ears flap like a machine.
#
# ── What the baking costs, measured ────────────────────────────────────────
# kilnlib's make_action says keying densely "buys nothing" because the
# importer resamples at a fixed 60 Hz. That is true of the SAMPLE RATE and
# false of the file size, which is worth writing down because I assumed
# otherwise and then measured it. One character, all nine actions:
#
#   sample every 2 frames   33.5 KB      every 4 frames   28.6 KB
#   sample every 3 frames   33.4 KB      every 5 frames   29.2 KB
#                                        every 10 frames  24.4 KB
#
# So the importer keyframe-REDUCES: a straight line between two poses
# compresses back down to its endpoints and costs nothing, while an eased one
# does not, because the curve is real information. The extra bytes are the
# animation. `step` below is therefore a genuine quality/size dial, and 4 is
# where it sits: 16 samples across a 60-frame action, close enough to the
# smoothstep it approximates to be indistinguishable at 60 Hz, and 15%
# cheaper than sampling twice as often for no visible gain.

# Frames each floppy part trails the body by. Asymmetric between left and
# right so the pair never moves as one rigid unit.
LAG = {
    "ear_l": 3, "ear_r": 4,
    "nose": 2, "jaw": 2,
    "hand_l": 2, "hand_r": 3,
    "forearm_l": 1, "forearm_r": 1,
}

ZERO = (0.0, 0.0, 0.0)


def _lerp_pose(a, b, t, rest):
    out = {}
    for bone in set(a) | set(b):
        pa = a.get(bone, rest.get(bone, ZERO))
        pb = b.get(bone, rest.get(bone, ZERO))
        out[bone] = tuple(pa[i] + (pb[i] - pa[i]) * t for i in range(3))
    return out


def _sample(keys, f, rest):
    """Pose at (possibly fractional) frame `f`, eased.

    A key may carry a third element naming the easing of the segment that
    ARRIVES at it — "out" to decelerate in, "over" to overshoot and settle,
    "in" to accelerate away from the previous pose. Default is smoothstep.
    """
    if f <= keys[0][0]:
        return keys[0][1]
    if f >= keys[-1][0]:
        return keys[-1][1]
    for i in range(len(keys) - 1):
        f0, p0 = keys[i][0], keys[i][1]
        f1, p1 = keys[i + 1][0], keys[i + 1][1]
        if f0 <= f <= f1:
            mode = keys[i + 1][2] if len(keys[i + 1]) > 2 else "inout"
            t = (f - f0) / (f1 - f0) if f1 > f0 else 1.0
            return _lerp_pose(p0, p1, m.ease(t, mode), rest)
    return keys[-1][1]


def _bake(armature, name, keys, length, rest=None, loop=True, step=4):
    """Turn animator keyframes into the dense eased keys the exporter wants."""
    rest = rest or {}
    bones = sorted({b for k in keys for b in k[1]})
    frames = list(range(0, length + 1, step))
    if frames[-1] != length:
        frames.append(length)

    channels = {}
    for bone in bones:
        lag = LAG.get(bone, 0)
        ks = []
        for f in frames:
            src = f - lag
            # A looping action wraps, so a lagged ear at frame 0 shows what
            # the head was doing at the END of the previous cycle — which is
            # the same cycle, so the loop stays seamless. A one-shot clamps
            # instead, or the settle would be preceded by the impact.
            src = src % length if loop else max(0, min(length, src))
            pose = _sample(keys, src, rest)
            ks.append((f, {'rot': pose.get(bone, rest.get(bone, ZERO))}))
        channels[bone] = ks
    return m.make_action(armature, name, channels, length=length)


# ── standing set ───────────────────────────────────────────────────────────

def anim_idle(armature):
    """Breathing, a weight shift, an ear twitch and a nose that will not sit
    still. Idle is what he does most of the time, so it carries most of the
    personality — and it is deliberately NOT symmetric in time: the weight
    shift takes longer to go than to come back, which is what stops a two-key
    breath reading as a metronome."""
    _bake(armature, "Idle", [
        (0,  {"torso": (0, 0, 0), "head": (0, 0, 0), "root": (0, 0, 0),
              "ear_l": (0, 0, 0), "ear_r": (0, 0, 0), "nose": (0, 0, 0),
              "arm_l": (0, 0, 0), "arm_r": (0, 0, 0), "jaw": (0, 0, 0)}),
        (22, {"torso": (3, 0, 1), "head": (-3, 0, -3), "root": (0, 0, 2),
              "ear_l": (0, -18, 0), "ear_r": (0, 8, 0), "nose": (-5, 0, 0),
              "arm_l": (-5, 0, 2), "arm_r": (-4, 0, -1), "jaw": (0, 0, 0)},
         "out"),
        (34, {"torso": (2, 0, 1), "head": (-1, 0, -2), "root": (0, 0, 2),
              "ear_l": (0, 4, 0), "ear_r": (0, -4, 0), "nose": (2, 0, 0),
              "arm_l": (-3, 0, 2), "arm_r": (-2, 0, -1), "jaw": (6, 0, 0)}),
        (46, {"torso": (1, 0, -1), "head": (2, 0, 3), "root": (0, 0, -2),
              "ear_l": (0, 10, 0), "ear_r": (0, -12, 0), "nose": (3, 0, 0),
              "arm_l": (2, 0, -1), "arm_r": (3, 0, 1), "jaw": (0, 0, 0)},
         "over"),
        (60, {"torso": (0, 0, 0), "head": (0, 0, 0), "root": (0, 0, 0),
              "ear_l": (0, 0, 0), "ear_r": (0, 0, 0), "nose": (0, 0, 0),
              "arm_l": (0, 0, 0), "arm_r": (0, 0, 0), "jaw": (0, 0, 0)}),
    ], length=60)


def _step_pose(fwd, hips_z, roll, arm_sign):
    """One half of the walk cycle, parameterised by which leg leads.

    `fwd` +1 means the LEFT leg is forward. The four poses a walk needs are
    contact / down / passing / up, and the previous version of this action
    had only contact and passing — which is why it read as a wind-up toy
    rather than as weight moving from foot to foot.
    """
    s = fwd
    return {
        "root":    (0, 0, roll),
        "torso":   (2, 0, -roll * 0.5),
        "neck":    (-1, 0, roll * 0.3),
        "head":    (0, 0, -roll * 0.8),
        "thigh_l": (28 * s, 0, 0), "thigh_r": (-28 * s, 0, 0),
        "shin_l":  (-6 - 14 * max(0, -s), 0, 0),
        "shin_r":  (-6 - 14 * max(0, s), 0, 0),
        "foot_l":  (-14 * s, 0, 0), "foot_r": (14 * s, 0, 0),
        "arm_l":   (-30 * s * arm_sign, 0, 4),
        "arm_r":   (30 * s * arm_sign, 0, -4),
        "forearm_l": (-16 - 10 * s, 0, 0),
        "forearm_r": (-16 + 10 * s, 0, 0),
        "ear_l":   (0, -12 * s, 0), "ear_r": (0, 12 * s, 0),
        "nose":    (-3 * s, 0, 0),
    }


def anim_walk(armature):
    """A waddle, as a proper four-pose cycle per step.

    contact -> down -> passing -> up, twice, mirrored. The hips drop on the
    down pose and rise on the push-off, which is the whole difference between
    walking and sliding; the shoulders counter-rotate against the hips; and
    the knee is never allowed to bend the wrong way, which is what the
    max(0, ...) terms in _step_pose are for.
    """
    def half(base, s):
        contact = _step_pose(s, 0, 5 * s, 1)
        down = dict(_step_pose(s * 0.5, -0.06, 6 * s, 1))
        down["root"] = (0, 0, 6 * s)
        passing = _step_pose(0, 0, 0, 1)
        passing["shin_l"] = (-30 if s > 0 else -6, 0, 0)
        passing["shin_r"] = (-30 if s < 0 else -6, 0, 0)
        up = _step_pose(-s * 0.4, 0.04, -3 * s, 1)
        return [(base + 0, contact), (base + 5, down, "out"),
                (base + 10, passing), (base + 15, up, "over")]

    keys = half(0, 1) + half(20, -1)
    keys.append((40, _step_pose(1, 0, 5, 1)))
    _bake(armature, "Walk", keys, length=40)


def anim_wave(armature):
    """Right arm up, hand flapping, head tilted.

    Anticipation first: the arm dips slightly BEFORE it rises. Two frames of
    it, and it is the difference between an arm that lifts and an arm that
    decides to lift. Deliberately asymmetric — a symmetric pose would not
    tell you whether left and right bones got swapped in the export.
    """
    down = {"arm_r": (0, 0, 0), "forearm_r": (0, 0, 0), "hand_r": (0, 0, 0),
            "head": (0, 0, 0), "torso": (0, 0, 0), "ear_r": (0, 0, 0),
            "ear_l": (0, 0, 0)}
    anticipate = {"arm_r": (0, 0, -14), "forearm_r": (0, 0, -6),
                  "hand_r": (0, 0, -8), "head": (0, 0, 4),
                  "torso": (0, 0, 3), "ear_r": (0, 8, 0), "ear_l": (0, -4, 0)}

    def up(flap, lean):
        return {"arm_r": (0, 0, 128), "forearm_r": (0, 0, 30),
                "hand_r": (0, 0, flap), "head": (0, 0, -14 + lean),
                "torso": (0, 0, -6), "ear_r": (0, -20, 0), "ear_l": (0, 9, 0)}

    _bake(armature, "Wave", [
        (0, down),
        (5, anticipate, "out"),
        (14, up(30, 0), "over"),
        (21, up(-28, -2)),
        (28, up(32, 1)),
        (35, up(-26, -2)),
        (42, up(10, 0)),
        (50, down, "inout"),
    ], length=50, loop=False)


def anim_taunt(armature):
    """Leans in, flaps both ears, works the jaw, wiggles.

    The busiest action, and therefore the one that will show a bone-order or
    channel-mapping bug soonest. It opens with a rock BACK before the lean
    forward — the same anticipation as the wave, at body scale.
    """
    rest = {"root": (0, 0, 0), "torso": (0, 0, 0), "head": (0, 0, 0),
            "jaw": (0, 0, 0), "ear_l": (0, 0, 0), "ear_r": (0, 0, 0),
            "arm_l": (0, 0, 0), "arm_r": (0, 0, 0), "nose": (0, 0, 0)}

    def lean(amount, jaw, wag):
        return {"root": (-amount, 0, wag), "torso": (-amount * 0.7, 0, -wag),
                "head": (amount * 1.4, 0, wag * 1.5), "jaw": (jaw, 0, 0),
                "ear_l": (0, -30 - wag * 2, 0), "ear_r": (0, 30 - wag * 2, 0),
                "arm_l": (0, 0, -48), "arm_r": (0, 0, 48),
                "nose": (12, 0, 0)}

    _bake(armature, "Taunt", [
        (0, rest),
        (7, {"root": (7, 0, 0), "torso": (5, 0, 0), "head": (-8, 0, 0),
             "jaw": (0, 0, 0), "ear_l": (0, 14, 0), "ear_r": (0, -14, 0),
             "arm_l": (0, 0, 8), "arm_r": (0, 0, -8), "nose": (-6, 0, 0)},
         "out"),
        (16, lean(18, 34, 0), "over"),
        (26, lean(16, 4, 7)),
        (34, lean(17, 32, -7)),
        (42, lean(15, 6, 5)),
        (60, rest, "inout"),
    ], length=60, loop=False)



# ── action set: run, jump, fall, land, attack, roll ────────────────────────
# Measured, not eyeballed: tools/blender/gait.py poses these through the same
# rest skeleton the glTF ships and reports each locomotion clip's GROUND SPEED
# (how fast a planted foot passes backwards), which is the number a game needs
# to play the clip without the feet skating. The numbers are published below
# and nix/checks/goblin-gait.nix holds both the clips and the C that plays
# them to it.
#
# The directions these poses use, measured by FK over the rig rather than
# guessed (model +X is his left, -Z is forward):
#   thigh +X   leg forward        shin -X    knee bends, foot back and up
#   foot  -X   toe up             root/torso -X   pitch forward
#   head  +X   chin up            arm +X     arm swings forward and up (both)
#   arm_r +Z / arm_l -Z   arm raised out to the side and up
#   forearm +X   elbow curls the hand forward
#   torso +Y   twists the right shoulder back

def _run_pose(s):
    """Contact, left leg leading when s = +1. A run has no double support, so
    the back leg is already pushing off as the front one lands."""
    return {
        "root":    (-10, 0, 4 * s),
        "torso":   (-8, 10 * s, -2 * s),
        "neck":    (6, 0, 0),
        "head":    (10, -6 * s, 0),
        "thigh_l": (46 * s, 0, 0), "thigh_r": (-46 * s, 0, 0),
        "shin_l":  (-12 - 44 * max(0, -s), 0, 0),
        "shin_r":  (-12 - 44 * max(0, s), 0, 0),
        "foot_l":  (-12 * s + 16 * max(0, -s), 0, 0),
        "foot_r":  (12 * s + 16 * max(0, s), 0, 0),
        "arm_l":   (-48 * s, 0, -6), "arm_r": (48 * s, 0, 6),
        "forearm_l": (64 + 12 * s, 0, 0), "forearm_r": (64 - 12 * s, 0, 0),
        "ear_l":   (0, -26, 0), "ear_r": (0, 26, 0),
        "nose":    (-4 * s, 0, 0), "jaw": (6, 0, 0),
    }


def anim_run(armature):
    """Sixteen frames, two strides: contact, down, passing, flight.

    The passing pose is where a run lives or dies. The SWING leg tucks — knee
    high, heel up under the backside — while the stance leg straightens under
    the body. Bend the stance knee instead and both feet pass at the same
    height, which gait.py reports as zero clearance and which reads on screen
    as a shuffle at speed.
    """
    def half(base, s):
        contact = _run_pose(s)
        down = dict(_run_pose(s * 0.7))
        down["root"] = (-14, 0, 5 * s)
        swing, stance = ("r", "l") if s > 0 else ("l", "r")
        passing = dict(_run_pose(0))
        passing.update({
            f"thigh_{swing}": (34, 0, 0), f"shin_{swing}": (-104, 0, 0),
            f"foot_{swing}": (28, 0, 0),
            f"thigh_{stance}": (-14, 0, 0), f"shin_{stance}": (-8, 0, 0),
            f"foot_{stance}": (8, 0, 0),
            "root": (-12, 0, 2 * s),
        })
        flight = dict(_run_pose(-s * 0.85))
        flight[f"shin_{stance}"] = (-30, 0, 0)
        flight[f"foot_{stance}"] = (34, 0, 0)
        return [(base + 0, contact), (base + 2, down, "out"),
                (base + 4, passing), (base + 6, flight, "in")]

    keys = half(0, 1) + half(8, -1)
    keys.append((16, _run_pose(1)))
    _bake(armature, "Run", keys, length=16, step=1)


def _tuck(amount):
    k = amount
    return {
        "root": (-24 * k, 0, 0), "torso": (-26 * k, 0, 0), "neck": (-10 * k, 0, 0),
        "head": (-16 * k, 0, 0),
        "thigh_l": (84 * k, 0, 6 * k), "thigh_r": (84 * k, 0, -6 * k),
        "shin_l": (-118 * k, 0, 0), "shin_r": (-118 * k, 0, 0),
        "foot_l": (30 * k, 0, 0), "foot_r": (30 * k, 0, 0),
        "arm_l": (62 * k, 0, 18 * k), "arm_r": (62 * k, 0, -18 * k),
        "forearm_l": (96 * k, 0, 0), "forearm_r": (96 * k, 0, 0),
        "ear_l": (0, 34 * k, 0), "ear_r": (0, -34 * k, 0),
    }


REST_BODY = {b: ZERO for b in _tuck(1)}


def anim_jump(armature):
    """The take-off, from the moment the feet leave the ground: fully extended
    — arms flung up, toes pointed — then the knees come up under him. A game
    starts this on the jump press and hands over to Fall at the apex."""
    extend = {
        "root": (6, 0, 0), "torso": (8, 0, 0), "neck": (4, 0, 0), "head": (12, 0, 0),
        "thigh_l": (-8, 0, 0), "thigh_r": (-4, 0, 0),
        "shin_l": (-4, 0, 0), "shin_r": (-10, 0, 0),
        "foot_l": (38, 0, 0), "foot_r": (34, 0, 0),
        "arm_l": (118, 0, -24), "arm_r": (118, 0, 24),
        "forearm_l": (10, 0, 0), "forearm_r": (10, 0, 0),
        "ear_l": (0, -40, 0), "ear_r": (0, 40, 0), "jaw": (18, 0, 0),
    }
    tuck = _tuck(0.6)
    tuck.update({"arm_l": (70, 0, -40), "arm_r": (70, 0, 40), "head": (4, 0, 0),
                 "jaw": (4, 0, 0), "ear_l": (0, 20, 0), "ear_r": (0, -20, 0)})
    _bake(armature, "Jump", [
        (0, dict(REST_BODY, **{"jaw": ZERO})),
        (3, extend, "out"),
        (12, tuck, "over"),
    ], length=12, loop=False)


def anim_fall(armature):
    """Arms out for balance, legs cycling a little — the loop he holds from
    the apex down. Small amplitude on purpose: it plays for as long as the
    fall lasts, and anything big becomes a flail."""
    def f(sw):
        return {
            "root": (4, 0, 2 * sw), "torso": (2, 0, -3 * sw), "head": (-10, 0, 3 * sw),
            "arm_l": (24 + 6 * sw, 0, -74 - 8 * sw), "arm_r": (24 - 6 * sw, 0, 74 - 8 * sw),
            "forearm_l": (26, 0, 0), "forearm_r": (26, 0, 0),
            "thigh_l": (22 + 12 * sw, 0, -6), "thigh_r": (10 - 12 * sw, 0, 6),
            "shin_l": (-40 - 10 * sw, 0, 0), "shin_r": (-40 + 10 * sw, 0, 0),
            "foot_l": (16, 0, 0), "foot_r": (16, 0, 0),
            "ear_l": (0, -34 - 10 * sw, 0), "ear_r": (0, 34 + 10 * sw, 0), "jaw": (14, 0, 0),
        }
    _bake(armature, "Fall", [(0, f(1)), (12, f(-1)), (24, f(1))], length=24)


def anim_land(armature):
    """Touchdown: absorb into a crouch, hold a beat, come up. The crouch RAISES
    the feet — there is no root translation channel — so a game lowers the
    body by the feet's rise while this plays (kiln_skel_bone_pos on the foot
    bones), which is also what keeps Idle and Walk planted."""
    impact = _tuck(0.55)
    impact.update({"head": (8, 0, 0), "arm_l": (34, 0, -30), "arm_r": (34, 0, 30),
                   "forearm_l": (30, 0, 0), "forearm_r": (30, 0, 0)})
    _bake(armature, "Land", [
        (0, _tuck(0.25)),
        (3, impact, "out"),
        (6, _tuck(0.5)),
        (14, REST_BODY, "inout"),
    ], length=14, loop=False)


# The bones Attack keys. Upper body only, by construction: a game masks the
# overlay to kiln_skel_mask_bone("torso") and the legs keep running under it,
# so a key on a leg or the root here would simply be thrown away.
ATTACK_BONES = ("torso", "neck", "head", "jaw", "arm_l", "forearm_l", "arm_r",
                "forearm_r", "hand_r", "ear_l", "ear_r")


def anim_attack(armature):
    """An overhead chop with the right hand. Anticipation (up and back, the
    torso winding away), a fast strike ("in" — accelerating into the hit),
    an overshoot past the target, and a slower recovery. Twelve frames: a game
    with a shorter attack window plays it faster, never cuts it."""
    def pose(**kw):
        return {b: kw.get(b, ZERO) for b in ATTACK_BONES}

    windup = pose(torso=(10, 26, 4), neck=(0, 6, 0), head=(-4, -12, 0), jaw=(10, 0, 0),
                  arm_r=(-24, 0, 148), forearm_r=(72, 0, 0), hand_r=(20, 0, 0),
                  arm_l=(30, 0, -26), forearm_l=(40, 0, 0),
                  ear_l=(0, 18, 0), ear_r=(0, -18, 0))
    strike = pose(torso=(-20, -26, -4), neck=(0, -6, 0), head=(-10, 10, 0), jaw=(24, 0, 0),
                  arm_r=(82, 0, 34), forearm_r=(6, 0, 0), hand_r=(-16, 0, 0),
                  arm_l=(-30, 0, -14), forearm_l=(20, 0, 0),
                  ear_l=(0, -30, 0), ear_r=(0, 30, 0))
    follow = pose(torso=(-24, -32, -4), neck=(0, -8, 0), head=(-12, 12, 0), jaw=(8, 0, 0),
                  arm_r=(92, 0, 22), forearm_r=(12, 0, 0), hand_r=(-24, 0, 0),
                  arm_l=(-34, 0, -12), forearm_l=(24, 0, 0))
    _bake(armature, "Attack", [
        (0, pose()),
        (3, windup, "out"),
        (5, strike, "in"),
        (7, follow, "over"),
        (12, pose(), "inout"),
    ], length=12, loop=False, step=1)


def anim_roll(armature):
    """The tucked ball a dodge roll spins. The SPIN is not here: the rig has
    rotation channels only, and a rotation about the feet-level origin would
    fell him rather than roll him. A game turns the whole body about
    ROLL_PIVOT_M while this holds the tuck."""
    _bake(armature, "Roll", [
        (0, REST_BODY),
        (3, _tuck(1.0), "out"),
        (10, _tuck(1.0)),
        (14, _tuck(0.2), "inout"),
    ], length=14, loop=False)


# Published for the C that plays these clips; nix/checks/goblin-gait.nix
# measures the clips and checks both these and the demos against them.
HIP_M = 0.72            # root bone height above the feet (BONES)
ROLL_PIVOT_M = 0.45     # centre of the tucked ball, above the feet


# ── riding ─────────────────────────────────────────────────────────────────
# ONE set of riding actions, meant to work across more than one vehicle body
# (a downstream game paired this rig with two: a kart and a motorcycle), by
# every one of the four characters.
#
# Two separate things make that work. Across CHARACTERS it is the shared rig
# at the top of this file. Across VEHICLES it is a shared convention (kept in
# that vehicle's own module, now in its own repo): every vehicle fixes a hip
# point and then places its grips and footrests at the same offsets from it,
# so a pose that reaches one vehicle's wheel and pedals reaches another's
# bars and pegs without a single value changing. A motorcycle built to that
# convention wants forward controls (ape-hanger, chopper-style), which is the
# one motorcycle ergonomic that genuinely matches a kart's.
#
# The alternative was riding animations per vehicle per character — eight sets
# differing only in arm angles, which doubles and redoubles the authoring,
# costs ROM for every copy, and guarantees they drift apart the first time one
# is tweaked.
#
# RIDE is the base pose every riding action departs from. Actions REPLACE
# rather than add, so each one carries the base pose in full — hence _ride(),
# which merges a per-action delta over it instead of restating it.
#
# The arm and leg angles here are SOLVED, not eyeballed: a grid search over
# the shoulder/elbow and hip/knee angles minimising the distance from the
# hand tip and ankle to the vehicle's own published grip/footrest points. A
# test re-measuring that distance on every run is what makes a change to
# either the rig or the vehicle's station fail loudly instead of showing up
# as hands hovering next to a wheel.
RIDE = {
    # The spine bones point UP and the limb bones point DOWN, so the SAME
    # local-X rotation swings them opposite ways in the world. That is why
    # the spine's forward lean is negative here and the limbs' forward reach
    # is positive, and it is not a typo — it caught me once already.
    "root":      (-8, 0, 0),    # leaning in toward the controls; every degree
    "torso":     (-5, 0, 0),    # the other way moves the shoulders back, and
    "neck":      (7, 0, 0),     # the arm chain has a fixed length
    "head":      (4, 0, 0),
    "arm_l":     (-9, 0, 67),
    "arm_r":     (-9, 0, -67),
    "forearm_l": (98, 0, 12),
    "forearm_r": (98, 0, -12),
    "hand_l":    (-20, 0, 0),   # wrist cocked over the grip
    "hand_r":    (-20, 0, 0),
    "thigh_l":   (90, 0, 7),
    "thigh_r":   (90, 0, -7),
    "shin_l":    (-43, 0, 0),
    "shin_r":    (-43, 0, 0),
    "foot_l":    (-30, 0, 0),
    "foot_r":    (-30, 0, 0),
    "jaw":       (0, 0, 0),
    "ear_l":     (0, -30, 0),   # ears blown back by the wind
    "ear_r":     (0, 30, 0),
    "nose":      (0, 0, 0),
}


def _ride(**deltas):
    """RIDE with per-bone (dx, dy, dz) degree offsets applied. Returns a
    POSE, not keys; _bake turns (frame, pose) pairs into channels."""
    out = dict(RIDE)
    for bone, delta in deltas.items():
        base = out.get(bone, ZERO)
        out[bone] = tuple(base[i] + delta[i] for i in range(3))
    return out


def anim_ride(armature):
    """Sitting at the controls, engine running.

    A small bob at the hips, the head drifting, ears fluttering — enough that
    a parked vehicle is not a statue, little enough that it never fights a
    steering pose. The ears are on a faster cycle than the body and lag it by
    a few frames, so the flutter reads as wind rather than as the body's own
    rhythm.
    """
    _bake(armature, "Ride", [
        (0,  _ride()),
        (12, _ride(root=(2, 0, 0), torso=(1, 0, 0), head=(-3, 0, 2),
                   ear_l=(0, -9, 0), ear_r=(0, 7, 0), nose=(-2, 0, 0)),
         "out"),
        (26, _ride(root=(-1, 0, 1), head=(1, 0, -1),
                   ear_l=(0, 11, 0), ear_r=(0, -9, 0), nose=(1, 0, 0))),
        (40, _ride(root=(2, 0, -1), torso=(1, 0, 0), head=(-2, 0, 2),
                   ear_l=(0, -7, 0), ear_r=(0, 6, 0), nose=(-1, 0, 0)),
         "over"),
        (52, _ride(root=(-1, 0, 0), head=(1, 0, -1),
                   ear_l=(0, 6, 0), ear_r=(0, -5, 0))),
        (60, _ride()),
    ], length=60)


def anim_ride_drive(armature):
    """Rolling. The counterpart of the vehicle's RideDrive, same 40 frames.

    The body absorbs the road: hips and torso ride a bob that peaks where the
    chassis pitches, but the HEAD stays level. That opposition is the whole
    trick — a rider whose head bobs in phase with the vehicle looks welded to
    it, and a rider whose head holds still while everything under him moves
    looks like a person on a machine.

    The arms stay locked to the grips throughout: they are the one thing that
    must not bob, because they are attached to something that is bobbing
    differently. Only the wrists give, which is what a real rider's do.
    """
    _bake(armature, "RideDrive", [
        (0,  _ride()),
        (7,  _ride(root=(3, 0, -1), torso=(2, 0, 0), neck=(-2, 0, 0),
                   head=(-1, 0, 1), hand_l=(-5, 0, 0), hand_r=(-4, 0, 0),
                   ear_l=(0, -10, 0), ear_r=(0, 9, 0), nose=(-3, 0, 0)),
         "out"),
        (14, _ride(root=(-2, 0, 1), torso=(-1, 0, 0), neck=(2, 0, 0),
                   head=(1, 0, -1), hand_l=(4, 0, 0), hand_r=(3, 0, 0),
                   ear_l=(0, 8, 0), ear_r=(0, -7, 0), nose=(2, 0, 0))),
        (22, _ride(root=(4, 0, 1), torso=(3, 0, 0), neck=(-3, 0, 0),
                   head=(-1, 0, -1), hand_l=(-6, 0, 0), hand_r=(-5, 0, 0),
                   ear_l=(0, -12, 0), ear_r=(0, 11, 0), nose=(-4, 0, 0)),
         "over"),
        (30, _ride(root=(-1, 0, -1), torso=(-1, 0, 0), neck=(1, 0, 0),
                   head=(1, 0, 1), hand_l=(3, 0, 0), hand_r=(2, 0, 0),
                   ear_l=(0, 7, 0), ear_r=(0, -6, 0), nose=(1, 0, 0))),
        (40, _ride()),
    ], length=40)


def _lean(sign, amount=1.0):
    """A steering lean. `sign` +1 leans to his left, -1 to his right.

    Both arms move together — the inside arm pulls back and down, the outside
    one pushes forward — because that is what turning a wheel or counter-
    steering a bar actually does to a rider's shoulders, and doing it to one
    arm only reads as reaching for something rather than as steering. The
    head counter-rotates against the body: he looks THROUGH the turn.
    """
    a = amount
    return _ride(
        root=(0, 0, 9 * sign * a),
        torso=(0, 0, 7 * sign * a),
        neck=(0, 0, -4 * sign * a),
        head=(0, 0, -11 * sign * a),
        arm_l=(-10 * sign * a, 0, 6 * sign * a),
        arm_r=(-10 * sign * a, 0, 6 * sign * a),
        forearm_l=(-6 * sign * a, 0, 0),
        forearm_r=(6 * sign * a, 0, 0),
        thigh_l=(0, 0, 3 * sign * a),
        thigh_r=(0, 0, 3 * sign * a),
        ear_l=(0, -14 * sign * a, 0),
        ear_r=(0, -14 * sign * a, 0),
    )


def _ride_steer(armature, name, sign):
    """Eases in, overshoots slightly, settles, and HOLDS.

    Held rather than looping so the game can start it on a stick deflection
    and stop it on release without needing to know where in the action it is.
    The counter-lean at frame 3 is anticipation: a rider shifts away from a
    turn for an instant before committing to it, and without those three
    frames the lean reads as the whole vehicle being nudged sideways.
    """
    _bake(armature, name, [
        (0,  _ride()),
        (3,  _lean(-sign, 0.18), "out"),
        (12, _lean(sign, 1.06), "over"),
        (18, _lean(sign, 1.0)),
        (30, _lean(sign, 1.0)),
    ], length=30, loop=False)


def anim_ride_left(armature):
    _ride_steer(armature, "RideLeft", 1)


def anim_ride_right(armature):
    _ride_steer(armature, "RideRight", -1)


def anim_ride_boost(armature):
    """Whoop.

    Rocks BACK for four frames, then folds forward over the controls with an
    overshoot, jaw open, ears streaming flat back. The rock-back is what
    sells the launch: it is the only part of the move the player's eye reads
    as effort, and without it the boost looks like the vehicle moved and the
    rider came along.
    """
    _bake(armature, "RideBoost", [
        (0, _ride()),
        (4, _ride(root=(-9, 0, 0), torso=(-7, 0, 0), neck=(5, 0, 0),
                  head=(6, 0, 0), jaw=(8, 0, 0),
                  arm_l=(8, 0, 0), arm_r=(8, 0, 0),
                  ear_l=(0, 16, 0), ear_r=(0, -16, 0)), "out"),
        (11, _ride(root=(18, 0, 0), torso=(16, 0, 0), neck=(-9, 0, 0),
                   head=(-5, 0, 0), jaw=(32, 0, 0),
                   arm_l=(-14, 0, 0), arm_r=(-14, 0, 0),
                   forearm_l=(11, 0, 0), forearm_r=(11, 0, 0),
                   ear_l=(0, -30, 0), ear_r=(0, 30, 0)), "over"),
        (24, _ride(root=(14, 0, 0), torso=(12, 0, 0), neck=(-8, 0, 0),
                   head=(-2, 0, 0), jaw=(22, 0, 0),
                   arm_l=(-10, 0, 0), arm_r=(-10, 0, 0),
                   forearm_l=(8, 0, 0), forearm_r=(8, 0, 0),
                   ear_l=(0, -34, 0), ear_r=(0, 34, 0))),
        (32, _ride(root=(10, 0, 0), torso=(8, 0, 0), jaw=(10, 0, 0),
                   ear_l=(0, -22, 0), ear_r=(0, 22, 0))),
        (46, _ride(), "inout"),
    ], length=46, loop=False)


def anim_ride_hit(armature):
    """Spun out.

    NO anticipation — that is the point. Anticipation is what a character
    does when they choose to move, and being hit is the one thing here that
    is not chosen, so frame 0 goes straight to the impact with "in" easing
    and the recovery oscillates down from there. Hands come OFF the controls,
    the only place a riding action is allowed to break the grip, and the
    reason it reads as losing control rather than as a wobble.
    """
    _bake(armature, "RideHit", [
        (0, _ride()),
        (4, _ride(root=(-16, 0, -14), torso=(-14, 0, -9), neck=(15, 0, 0),
                  head=(20, 0, 16), jaw=(38, 0, 0),
                  arm_l=(-42, 0, -56), arm_r=(-42, 0, 56),
                  forearm_l=(-32, 0, 0), forearm_r=(-32, 0, 0),
                  hand_l=(42, 0, 0), hand_r=(42, 0, 0),
                  thigh_l=(-18, 0, 14), thigh_r=(-18, 0, -14),
                  ear_l=(0, 48, 0), ear_r=(0, -48, 0)), "in"),
        (16, _ride(root=(9, 0, 16), torso=(7, 0, 11), neck=(-11, 0, 0),
                   head=(-13, 0, -18), jaw=(30, 0, 0),
                   arm_l=(-34, 0, -46), arm_r=(-34, 0, 46),
                   forearm_l=(-25, 0, 0), forearm_r=(-25, 0, 0),
                   hand_l=(31, 0, 0), hand_r=(31, 0, 0),
                   thigh_l=(-12, 0, -11), thigh_r=(-12, 0, 11),
                   ear_l=(0, -42, 0), ear_r=(0, 42, 0)), "over"),
        (27, _ride(root=(-5, 0, -8), torso=(-4, 0, -5), head=(7, 0, 8),
                   jaw=(18, 0, 0),
                   arm_l=(-18, 0, -24), arm_r=(-18, 0, 24),
                   hand_l=(14, 0, 0), hand_r=(14, 0, 0),
                   ear_l=(0, 24, 0), ear_r=(0, -24, 0))),
        (36, _ride(root=(2, 0, 4), head=(-3, 0, -4), jaw=(7, 0, 0),
                   arm_l=(-7, 0, -10), arm_r=(-7, 0, 10),
                   ear_l=(0, -12, 0), ear_r=(0, 12, 0))),
        (52, _ride(), "inout"),
    ], length=52, loop=False)
# ── signature poses ────────────────────────────────────────────────────────
# Every other action in this file is SHARED — the whole point of the
# byte-identical rig is that one Walk plays on all four goblins. This is the
# one deliberate exception: each character carries a different pose under the
# SAME action name, "Pose".
#
# That is what a character-select screen wants. The screen plays "Pose" on
# whoever is highlighted and does not care which goblin it is; the goblin
# supplies the personality. Sharing the name costs the game nothing and
# sharing the geometry would cost the characters everything — a line-up where
# all four stand identically is four recolours, not four characters.
#
# It is also one action, not four, so the ROM pays for exactly one more clip
# per character rather than four per character.
#
# ── One rule these poses are built to ──────────────────────────────────────
# Put the SPREAD in the upper arm's Z and the BEND in the forearm's X. A Z
# rotation on the forearm folds the hand back across the chest and cancels
# whatever the shoulder just did — the first pass gave Glimmer a grand
# presenting gesture on the shoulders and then quietly undid it at the
# elbows, and she rendered with both hands knotted under her chin.
POSES = {
    # The neutral build just stands there — it is the shape reference, not a
    # character, and a pose on it would be a lie about what it is for.
    "goblin": {},

    # Dank: poured backwards into a chair that is not there. Weight on the
    # back foot, head tipped back, one arm up in a peace sign he has not
    # got round to finishing. Everything reads as "in a minute".
    "dank": {
        "root": (9, 0, -5), "torso": (6, 0, 4), "neck": (-4, 0, -3),
        "head": (-6, 0, 9), "jaw": (7, 0, 0),
        "arm_r": (0, 0, 74), "forearm_r": (-8, 0, 80), "hand_r": (0, 0, 12),
        "arm_l": (-14, 0, -16), "forearm_l": (-20, 0, 0), "hand_l": (8, 0, 0),
        "thigh_l": (11, 0, -5), "shin_l": (-26, 0, 0), "foot_l": (10, 0, 0),
        "thigh_r": (-9, 0, 7), "shin_r": (-13, 0, 0),
        "ear_l": (0, 26, 0), "ear_r": (0, -26, 0), "nose": (6, 0, 0),
    },

    # Sparky: caught mid-launch. Folded forward over his own toes, arms flung
    # back behind him, jaw wide open. The only pose of the four with both
    # feet doing different things — he is already leaving.
    "sparky": {
        "root": (-23, 0, 2), "torso": (-15, 0, -2), "neck": (17, 0, 0),
        "head": (13, 0, -5), "jaw": (34, 0, 0),
        "arm_l": (58, 0, -32), "forearm_l": (-42, 0, 0), "hand_l": (-18, 0, 0),
        "arm_r": (54, 0, 28), "forearm_r": (-38, 0, 0), "hand_r": (-18, 0, 0),
        "thigh_l": (38, 0, -7), "shin_l": (-44, 0, 0), "foot_l": (-14, 0, 0),
        "thigh_r": (-27, 0, 7), "shin_r": (-52, 0, 0), "foot_r": (26, 0, 0),
        "ear_l": (0, -46, 0), "ear_r": (0, 46, 0), "nose": (-9, 0, 0),
    },

    # Moss: planted. Wide stance, arms hanging heavy and slightly out from a
    # body too wide for them to hang straight, head down under the brow. The
    # only one of the four whose pose is about NOT moving.
    "moss": {
        "root": (-7, 0, 0), "torso": (-5, 0, 0), "neck": (5, 0, 0),
        "head": (3, 0, 0), "jaw": (11, 0, 0),
        "arm_l": (7, 0, -50), "forearm_l": (14, 0, 6), "hand_l": (-14, 0, 0),
        "arm_r": (7, 0, 50), "forearm_r": (14, 0, -6), "hand_r": (-14, 0, 0),
        "thigh_l": (2, 0, -15), "shin_l": (-7, 0, 0), "foot_l": (4, 0, 0),
        "thigh_r": (2, 0, 15), "shin_r": (-7, 0, 0), "foot_r": (4, 0, 0),
        "ear_l": (0, 9, 0), "ear_r": (0, -9, 0),
    },

    # Glimmer: presenting. Hip cocked, one hand on it, the other swept out
    # as if the prize were already hers, chin up and turned away from the
    # gesture. The asymmetry is the whole read.
    "glimmer": {
        "root": (0, 0, 9), "torso": (3, 0, -6), "neck": (-2, 0, 5),
        "head": (-7, 0, -12), "jaw": (4, 0, 0),
        "arm_r": (0, 0, 112), "forearm_r": (-24, 0, 12), "hand_r": (18, 0, 0),
        "arm_l": (4, 0, -62), "forearm_l": (-16, 0, -8), "hand_l": (-8, 0, 0),
        "thigh_l": (9, 0, -3), "shin_l": (-11, 0, 0),
        "thigh_r": (-5, 0, 11), "shin_r": (-19, 0, 0), "foot_r": (14, 0, 0),
        "ear_l": (0, -15, 0), "ear_r": (0, 15, 0), "nose": (-4, 0, 0),
    },
}


def anim_pose(armature, pose):
    """The held signature pose, breathing.

    A one-frame pose would be a statue, and a character-select screen full of
    statues is the thing this whole animation pass exists to avoid. So the
    pose is the REST here and the action keys small deviations off it —
    exactly the shape the riding actions use, at a quarter of the amplitude.
    """
    def p(**deltas):
        out = dict(pose)
        for bone, delta in deltas.items():
            base = out.get(bone, ZERO)
            out[bone] = tuple(base[i] + delta[i] for i in range(3))
        return out

    if not pose:                    # the neutral build: reuse Idle's shape
        return

    _bake(armature, "Pose", [
        (0,  p()),
        (26, p(torso=(2, 0, 1), head=(-2, 0, 2), jaw=(3, 0, 0),
               arm_l=(-2, 0, 1), arm_r=(-2, 0, -1),
               ear_l=(0, -11, 0), ear_r=(0, 9, 0), nose=(-3, 0, 0)), "out"),
        (48, p(torso=(-1, 0, -1), head=(1, 0, -1),
               ear_l=(0, 8, 0), ear_r=(0, -7, 0), nose=(2, 0, 0))),
        (72, p()),
    ], length=72, rest=pose)


def build_all_actions(armature, spec_name="goblin"):
    anim_idle(armature)
    anim_walk(armature)
    anim_wave(armature)
    anim_taunt(armature)
    anim_run(armature)
    anim_jump(armature)
    anim_fall(armature)
    anim_land(armature)
    anim_attack(armature)
    anim_roll(armature)
    anim_pose(armature, POSES.get(spec_name, {}))
    anim_ride(armature)
    anim_ride_drive(armature)
    anim_ride_left(armature)
    anim_ride_right(armature)
    anim_ride_boost(armature)
    anim_ride_hit(armature)


def main():
    name = m.arg("--model", "goblin")
    if name not in GOBLINS:
        raise SystemExit(f"goblin.py: no character '{name}' "
                         f"(have: {', '.join(sorted(GOBLINS))})")

    m.reset_scene()
    armature = build_goblin(GOBLINS[name])
    build_all_actions(armature, name)

    # Raised from 280. The lofted masses, the segment() limbs, the elbow, the
    # knee, the solid ears and the per-character extras are all new, and each
    # one is there because the box-stack version either read badly or could
    # not hold a riding pose. Four of these on screen at once is ~2000 tris
    # before whatever vehicle they might be riding.
    # Author high, weld down. The reclaim is printed rather than assumed:
    # if it ever drops to nothing, the detail being added is already at or
    # below the console's 1/64 grid and is costing triangles for a shape the
    # hardware cannot represent.
    print(f"  [WELD] {WELD['verts_in']:>4} -> {WELD['verts_out']:>4} verts, "
          f"{WELD['tris_in']:>4} -> {WELD['tris_out']:>4} tris, "
          f"{WELD['dropped']} degenerate faces dropped")
    m.report(max_tris=900)
    m.export_gltf(m.arg("--out"), animated=True)


main()
