#!/usr/bin/env python3
"""
machine_centaur_gen.py — parametric N64-budget character model.

Subject : unnamed self-made machine.  A man who cut himself off at the waist
          and grafted the remainder onto a four-legged steel chassis.  The
          legs strike out horizontally from the hull before dropping to the
          deck, so he reads wide and low and heavy before you ever see the
          human half.  Left forearm is a break-action double barrel; three
          bare-steel claws are bolted round the muzzle where a hand was.

Target  : Nintendo 64, F3DEX2 microcode (32-vertex cache), vertex-colour
          gouraud shading + two 32x32 CI4 textures sharing one TLUT bank.

Design is table-driven.  LANDMARKS places every joint in centimetres, RADII
gives each joint its cross-section, and STANCE / GRAFT are scalar dials for
the two things you will actually want to tune: how far the legs splay, and
how raw the flesh-to-steel seam looks.  Change a number, rerun, and the OBJ,
the C header and the preview all stay in sync.

Emits:
  machine_centaur.obj     Wavefront OBJ (+ vertex colours, + UVs)
  machine_centaur.mtl     material stub
  machine_centaur.h       F3DEX2 Vtx[] arrays + per-material display lists
  mc_face.png             32x32 face texture (16-colour indexed)
  mc_plate.png            32x32 hull plate texture (16-colour indexed)

Usage:  python3 machine_centaur_gen.py [--scale 8] [--outdir .]
"""

import argparse
import math
import os

ANIMS = {}

# ---------------------------------------------------------------------------
# palette.  Nothing on this character is a clean colour.  The flesh has gone
# grey-green from poor perfusion; the steel is old, oxidised and streaked.
# ---------------------------------------------------------------------------
FLESH       = (162, 152, 132)   # sallow, drained
FLESH_DARK  = (120, 112,  96)
FLESH_COLD  = (134, 138, 128)   # the parts nearest the graft, going off
NECRO       = ( 96,  52,  54)   # the seam itself
GORE        = ( 86,  40,  40)   # meat: pleura, lung, the walls of the cavity
CLOT        = ( 44,  12,  16)   # old blood, gone black at the edges
BLOOD       = (108,  20,  22)   # wet, and still arriving
BLOOD_LIT   = (168,  42,  38)   # arterial, only where it is fresh
BONE_MARROW = (140,  76,  70)   # the face of a snapped rib
BONE        = (196, 188, 168)   # exposed rib ends

STEEL       = (108, 112, 118)
STEEL_DARK  = ( 68,  72,  78)
STEEL_LIT   = (146, 150, 156)
IRON_OX     = (118,  78,  52)   # rust bloom at the welds
GREASE      = ( 44,  44,  48)   # hydraulics, joint boots

BRASS       = (142, 116,  66)   # shotgun receiver
GUNMETAL    = ( 78,  80,  86)   # barrels
CLAW        = (196, 200, 204)   # kept sharp, therefore kept bright

OPTIC       = (198,  40,  34)   # the lenses where his left eye was
OPTIC_DK    = ( 96,  18,  16)   # glass in shadow, outside the hot core
OPTIC_HOT   = (255, 120,  92)   # the core, and the pip off the cover glass
CABLE       = ( 52,  50,  58)

VOID        = ( 10,   8,  10)   # aperture, socket, the inside of the mouth
BONE_DARK   = (150, 142, 122)
TEETH       = (232, 226, 206)
SCLERA      = (206, 198, 176)   # gone yellow — he does not blink much
IRIS        = ( 92, 108, 102)

# ---------------------------------------------------------------------------
# dials
# ---------------------------------------------------------------------------
STANCE = {
    "splay":     1.00,   # 0 = legs tucked under hull, 1 = full horizontal reach
    "ride":      1.00,   # hull height multiplier
    "toe_in":    0.00,   # rotates the foot pads inward, radians
}

GRAFT = {
    "raw":       1.00,   # 0 = healed seam, 1 = weeping, stapled, necrotic
    "open_ribs": 1.00,   # 0 = chest closed, 1 = left ribcage spread open
}

# ---------------------------------------------------------------------------
# landmarks.  Y up, +Z forward (he faces the camera), +X his left.
# Origin sits on the floor at the centre of the hull footprint.  Centimetres.
# ---------------------------------------------------------------------------
HULL = {
    "front_z":   52.0,
    "rear_z":   -62.0,
    "half_x":    25.0,                        # narrow, so the booms stand clear
    "floor_y":   82.0 * STANCE["ride"],
    "roof_y":   142.0 * STANCE["ride"],       # deep barrel, taller than wide
}

# Leg attachment points on the hull flank, and the chain out from each.
#
# Both roots moved back 22 cm for the overhand and the punch.  Those are the
# two moves that throw mass forward and down, and the torso already sits at
# z=+16 with a forward lean on top of that — every gram of him is ahead of
# centre before the arm even starts.  Dropping the rear foot from z=-104 to
# z=-126 lengthens the moment arm the chassis has to resist toppling with,
# and pulling the front root back gets the front feet under the strike instead
# of reaching past it.  balance_check() below reports the margin per frame.
LEG_ROOT_Z = {"front": 8.0, "rear": -62.0}
LEG_SWEEP  = {"front": 58.0, "rear": -64.0}   # fore/aft rake — kills the
                                              # "table leg" read in profile

# nodes in order: coxa root (high on the flank), boom tip, knee, ankle.
# The boom is deliberately near-level — that flat run is the whole read.
LEG_CHAIN = [
    # (dx from hull flank, dy from root,      dz fraction of sweep)
    (  0.0,    0.0, 0.00),   # root
    ( 46.0,    2.0, 0.34),   # boom tip — the horizontal reach
    ( 72.0,   34.0, 0.62),   # knee, cranked well above the hull roof
    ( 88.0, -122.0, 1.00),   # ankle, just off the deck
]
LEG_RADII = [17.0, 14.0, 11.5, 6.5]
LEG_ROOT_DROP = 18.0                          # below the roofline

# human half.  He was tall; the chassis made him taller.
TORSO = {
    "graft_y":  142.0 * STANCE["ride"],       # where flesh meets socket
    "waist_y":  160.0 * STANCE["ride"],
    "chest_y":  186.0 * STANCE["ride"],
    "yoke_y":   202.0 * STANCE["ride"],       # shoulder line
    "neck_y":   208.0 * STANCE["ride"],
    "chin_y":   217.0 * STANCE["ride"],
    "crown_y":  238.0 * STANCE["ride"],
    "torso_z":   16.0,                        # sits over the fore quarter
    "lean":       1.00,                       # forward pitch of the human half
}


def lean_z(y):
    """Forward drift applied to the human half — he does not stand straight."""
    t = max(0.0, (y - TORSO["graft_y"]) / 90.0)
    return TORSO["lean"] * 11.0 * t * t

# ---------------------------------------------------------------------------
# rig.  Rigid limb binding, one bone per vertex, exactly the way the N64 did
# it: no skinning, no weights — each limb is a closed piece of geometry and the
# matrix stack does the rest.  Joints are hidden by overlapping the child's
# proximal ring into the parent rather than by blending, because there is
# nothing to blend with.
#
# Pivots are filled in by build_bones() once the landmark maths has run, so the
# rig can never drift out of step with the geometry it is driving.
# ---------------------------------------------------------------------------
CUR_BONE = ["root"]
CHEST_HW = (0, 0)   # index span of the ribcage hardware, for clearance checks
BONES = {}          # name -> {"parent": name|None, "pivot": (x, y, z)}
BONE_ORDER = []     # parents always before children — the emit order


class bone:
    """`with bone("head"):` — everything added inside binds to that limb."""

    def __init__(self, name):
        self.name = name

    def __enter__(self):
        CUR_BONE.append(self.name)
        return self

    def __exit__(self, *a):
        CUR_BONE.pop()


def add_bone(name, parent, pivot):
    BONES[name] = {"parent": parent, "pivot": tuple(float(v) for v in pivot)}
    BONE_ORDER.append(name)


def leg_nodes(side, pair):
    """The four joint centres of one leg — shared by the mesh and the rig."""
    rootz = LEG_ROOT_Z[pair]
    sweep = LEG_SWEEP[pair]
    basey = HULL["roof_y"] - LEG_ROOT_DROP
    out = []
    for (dx, dy, dzf) in LEG_CHAIN:
        out.append((side * (HULL["half_x"] + dx * STANCE["splay"]),
                    basey + dy,
                    rootz + sweep * dzf * STANCE["splay"]))
    return out


def leg_name(side, pair):
    return "%s%s" % ("L" if side > 0 else "R", "F" if pair == "front" else "B")


def gun_frame():
    """The muzzle frame — shared by the mesh and the rig so the claw hinges
    land on the actual knuckles instead of near them."""
    tz = TORSO["torso_z"]
    el = (32.0, TORSO["chest_y"] - 20, tz + 6)
    mz = (38.0, TORSO["waist_y"] - 26, tz + 34)
    ax = norm(sub(mz, el))
    side = norm(cross((0.0, 1.0, 0.0), ax))
    up = cross(ax, side)
    return el, mz, ax, side, up


def claw_knuckle(k):
    _, mz, ax, side, up = gun_frame()
    ang = math.pi / 2 + k * (2 * math.pi / 3)
    radial = tuple(math.cos(ang) * side[j] + math.sin(ang) * up[j]
                   for j in range(3))
    return tuple(mz[j] - 5.0 * ax[j] + 8.5 * radial[j] for j in range(3))


def check_clearance():
    """Warn when chest hardware wanders into the gun arm.

    The retractor jaws reached x=36 once, which put them inside a humerus that
    runs from x=24 to x=32 — invisible in the rest pose from the front, obvious
    the moment the arm swings. Geometry that lives near another limb gets
    checked rather than eyeballed.
    """
    el, _, _, _, _ = gun_frame()
    sh = (24.0, TORSO["yoke_y"] - 2, TORSO["torso_z"])
    bad = []
    # Only the retractor.  The torso shell and the cut rim both legally come
    # within a few centimetres of the humerus — that is the pec meeting the
    # deltoid, and flagging it buries the one result that matters.
    lo, hi = CHEST_HW
    for i in range(lo, hi):
        x, y, z = M.v[i]
        if x < 10.0:
            continue
        t = max(0.0, min(1.0, (y - sh[1]) / (el[1] - sh[1])))
        ax_ = [sh[j] + (el[j] - sh[j]) * t for j in range(3)]
        d = math.sqrt((x - ax_[0]) ** 2 + (y - ax_[1]) ** 2 + (z - ax_[2]) ** 2)
        if d < 8.5:
            bad.append((i, round(d, 1)))
    if bad:
        print("  !! %d chest verts inside the gun arm (min %.1f cm)"
              % (len(bad), min(d for _, d in bad)))
    return bad


def balance_check(verbose=True):
    """Per-frame centre of mass against the planted-foot support polygon.

    "Consider the momentum" is not something you can eyeball on a four-legged
    machine with a man bolted to the front of it: the torso sits 16 cm ahead of
    centre and leans further, so he is nose-heavy before an arm moves.  An
    overhand and a straight right both throw mass forward and down, and if the
    CoM crosses the front feet the pose is one a real chassis would fall out of.

    Mass is approximated by triangle area rather than vertex count — the head
    carries a quarter of the model's vertices and about a twentieth of its
    volume, so a per-vertex mean puts the CoM up near his chin.

    Reports the signed Z margin: how far the CoM sits behind the front feet
    (positive is stable) and ahead of the rear feet, whichever is tighter.
    """
    import numpy as np

    V = np.array(M.v)
    tris, areas = [], []
    for (bn, _), ts in M.groups.items():
        for t in ts:
            tris.append(t)
    tris = np.array(tris)
    a, b, c = V[tris[:, 0]], V[tris[:, 1]], V[tris[:, 2]]
    areas = 0.5 * np.linalg.norm(np.cross(b - a, c - a), axis=1)
    bone_of = np.array(M.b)

    worst = {}
    for aname, anim in ANIMS.items():
        n = anim["frames"]
        margin = 1e9
        at = 0
        airborne = 0
        for f in range(0, n + 1, max(1, n // 24)):
            mats = pose_matrices(anim, f)
            P = np.empty_like(V)
            hom = np.hstack([V, np.ones((len(V), 1))])
            for nm, Mx in mats.items():
                sel = bone_of == nm
                if sel.any():
                    P[sel] = (Mx @ hom[sel].T).T[:, :3]
            cen = (P[tris[:, 0]] + P[tris[:, 1]] + P[tris[:, 2]]) / 3.0
            com = (cen * areas[:, None]).sum(0) / areas.sum()
            # planted feet only — a foot in the air supports nothing
            fz = []
            for side in (1, -1):
                for pair in ("front", "rear"):
                    sel = bone_of == (leg_name(side, pair) + "_shin")
                    pts = P[sel]
                    if pts[:, 1].min() < 12.0:
                        fz.append(pts[np.argmin(pts[:, 1]), 2])
            if len(fz) >= 2 and max(fz) - min(fz) < 12.0:
                # two contacts at the same z are one axle, not a base — the
                # margin formula would hand back a confident negative about a
                # pose that is simply mid-transition
                airborne += 1
                continue
            if len(fz) < 2:
                # Fewer than two contacts is not a balance failure, it is a
                # different state — a jump is *supposed* to leave the deck.
                # Reporting it as a topple would train me to ignore the check.
                airborne += 1
                continue
            else:
                m = min(max(fz) - com[2], com[2] - min(fz))
            if m < margin:
                margin, at = m, f
        worst[aname] = (margin, at)
        if verbose:
            flag = "  <-- tips" if margin < 0 else ""
            air = "  (%d airborne)" % airborne if airborne else ""
            print("  balance %-18s %+6.1f cm at frame %2d%s%s"
                  % (aname, margin, at, flag, air))
    return worst


def build_bones():
    add_bone("root", None, (0.0, 0.0, 0.0))
    add_bone("hull", "root", (0.0, (HULL["floor_y"] + HULL["roof_y"]) * 0.5, 0.0))
    for side in (1, -1):
        for pair in ("front", "rear"):
            n = leg_nodes(side, pair)
            g = leg_name(side, pair)
            add_bone(g + "_hip", "hull", n[0])
            add_bone(g + "_boom", g + "_hip", n[1])
            add_bone(g + "_shin", g + "_boom", n[2])
    add_bone("torso", "hull", (0.0, TORSO["graft_y"], TORSO["torso_z"]))
    add_bone("head", "torso", (0.0, 209.0, TORSO["torso_z"]))
    add_bone("jaw", "head",
             head_tilt((TMJ[0], TMJ[1], TORSO["torso_z"] + TMJ[2])))
    add_bone("armR", "torso", (-24.0, TORSO["yoke_y"] - 2, TORSO["torso_z"]))
    add_bone("armL", "torso", (24.0, TORSO["yoke_y"] - 2, TORSO["torso_z"]))
    add_bone("gun", "armL", (32.0, TORSO["chest_y"] - 20, TORSO["torso_z"] + 6))
    for k in range(3):
        add_bone("claw%d" % k, "gun", claw_knuckle(k))


# ---------------------------------------------------------------------------
# mesh container
# ---------------------------------------------------------------------------
class Mesh:
    def __init__(self):
        self.v = []          # (x, y, z)
        self.c = []          # (r, g, b)
        self.uv = []         # (u, v) in 0..1
        self.b = []          # bone name — rigid binding, one bone per vertex
        self.groups = {}     # (bone, material) -> list of (a, b, c) triples

    def add(self, p, col, uv=(0.0, 0.0)):
        self.v.append(tuple(float(x) for x in p))
        self.c.append(tuple(int(x) for x in col))
        self.uv.append((float(uv[0]), float(uv[1])))
        self.b.append(CUR_BONE[-1])
        return len(self.v) - 1

    def tri(self, group, a, b, c):
        self.groups.setdefault((self.b[a], group), []).append((a, b, c))

    def quad(self, group, a, b, c, d):
        self.tri(group, a, b, c)
        self.tri(group, a, c, d)

    def count(self):
        return sum(len(t) for t in self.groups.values())

    def by_material(self):
        out = {}
        for (bn, mat), tris in self.groups.items():
            out.setdefault(mat, []).extend(tris)
        return out


M = Mesh()


# ---------------------------------------------------------------------------
# primitives
# ---------------------------------------------------------------------------
def shade(col, k):
    """Multiply a colour, clamped.  Used for baked ambient occlusion."""
    return tuple(max(0, min(255, int(round(ch * k)))) for ch in col)


def ring(cx, cy, cz, rx, rz, n, col, phase=0.0, uv_v=0.0, ao=1.0):
    """A closed loop of n verts in the XZ plane at height cy."""
    out = []
    for i in range(n):
        a = phase + 2.0 * math.pi * i / n
        x = cx + rx * math.cos(a)
        z = cz + rz * math.sin(a)
        out.append(M.add((x, cy, z), shade(col, ao), (i / n, uv_v)))
    return out


def ring_at(center, axis, up, r, n, col, phase=0.0, uv_v=0.0, ao=1.0):
    """A loop of n verts on the plane perpendicular to `axis`."""
    ax = norm(axis)
    u = norm(cross(up, ax))
    if length(u) < 1e-6:
        u = norm(cross((1.0, 0.0, 0.0), ax))
    w = cross(ax, u)
    out = []
    for i in range(n):
        a = phase + 2.0 * math.pi * i / n
        p = tuple(center[k] + r * (math.cos(a) * u[k] + math.sin(a) * w[k])
                  for k in range(3))
        out.append(M.add(p, shade(col, ao), (i / n, uv_v)))
    return out


def skin(group, a, b, flip=False, skip=()):
    """Stitch two equal-length rings into a tube wall.

    `skip` omits segments by index, which is how the chest gets a hole rather
    than a decal: the ribcage has to open *away from* something, and a solid
    shell with blades stuck on the outside is what four flat quads looked like
    from any angle that mattered.
    """
    n = len(a)
    for i in range(n):
        if i in skip:
            continue
        j = (i + 1) % n
        if flip:
            M.quad(group, a[i], b[i], b[j], a[j])
        else:
            M.quad(group, a[i], a[j], b[j], b[i])


def cap(group, r, center, col, flip=False, ao=1.0):
    """Fan-close a ring to a centre point."""
    ci = M.add(center, shade(col, ao), (0.5, 0.5))
    n = len(r)
    for i in range(n):
        j = (i + 1) % n
        if flip:
            M.tri(group, ci, r[j], r[i])
        else:
            M.tri(group, ci, r[i], r[j])
    return ci


def box(group, lo, hi, col, ao_bottom=0.72):
    """Axis-aligned box, darker underneath so it grounds without a light rig."""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    top = shade(col, 1.0)
    bot = shade(col, ao_bottom)
    b = [M.add((x0, y0, z0), bot), M.add((x1, y0, z0), bot),
         M.add((x1, y0, z1), bot), M.add((x0, y0, z1), bot)]
    t = [M.add((x0, y1, z0), top), M.add((x1, y1, z0), top),
         M.add((x1, y1, z1), top), M.add((x0, y1, z1), top)]
    M.quad(group, t[0], t[1], t[2], t[3])
    M.quad(group, b[3], b[2], b[1], b[0])
    M.quad(group, b[0], b[1], t[1], t[0])
    M.quad(group, b[1], b[2], t[2], t[1])
    M.quad(group, b[2], b[3], t[3], t[2])
    M.quad(group, b[3], b[0], t[0], t[3])
    return b, t


# small vector helpers
def sub(a, b):  return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def length(a):  return math.sqrt(a[0] ** 2 + a[1] ** 2 + a[2] ** 2)
def norm(a):
    L = length(a)
    return (a[0] / L, a[1] / L, a[2] / L) if L > 1e-9 else (0.0, 1.0, 0.0)
def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])
def lerp3(a, b, t):
    return tuple(a[k] + (b[k] - a[k]) * t for k in range(3))


# ---------------------------------------------------------------------------
# 1. hull — an octagonal barrel lying along Z, the animal body of the centaur
# ---------------------------------------------------------------------------
def build_hull():
    fy, ry = HULL["floor_y"], HULL["roof_y"]
    cy = 0.5 * (fy + ry)
    ry_half = 0.5 * (ry - fy)
    hx = HULL["half_x"]

    # four cross-sections: tail, rear haunch, fore haunch, breast
    sections = [
        (HULL["rear_z"],        hx * 0.54, ry_half * 0.58, 0.74),
        (HULL["rear_z"] + 30,   hx * 1.00, ry_half * 0.98, 0.92),
        (HULL["front_z"] - 30,  hx * 1.00, ry_half * 1.00, 1.00),
        (HULL["front_z"],       hx * 0.66, ry_half * 0.72, 0.90),
    ]
    rings = []
    for i, (z, rx, ryy, ao) in enumerate(sections):
        loop = []
        for k in range(8):
            a = math.pi * (2 * k + 1) / 8.0          # phase so edges face out
            x = rx * math.cos(a)
            y = cy + ryy * math.sin(a)
            # underside is dirtier and darker
            k_ao = ao * (0.70 if math.sin(a) < -0.3 else 1.0)
            col = IRON_OX if (i in (1, 2) and math.sin(a) < -0.6) else STEEL
            loop.append(M.add((x, y, z), shade(col, k_ao),
                              (k / 8.0, 0.25 + 0.25 * i)))
        rings.append(loop)

    for i in range(len(rings) - 1):
        skin("hull", rings[i], rings[i + 1])
    cap("hull", rings[0], (0.0, cy, HULL["rear_z"] - 6), STEEL_DARK, flip=True)
    cap("hull", rings[-1], (0.0, cy, HULL["front_z"] + 8), STEEL_DARK)

    # dorsal channel: a narrow riveted trough he bolted on to carry the spine
    box("hull", (-8.0, ry - 6.0, HULL["rear_z"] + 14),
        (8.0, ry + 2.0, TORSO["torso_z"] - 4), STEEL_LIT)

    # exhaust stack at the rear quarter — he vents heat, not breath
    er = ring_at((14.0, ry - 6.0, HULL["rear_z"] + 22), (0, 1, 0), (0, 0, 1),
                 6.0, 6, GREASE)
    er2 = ring_at((16.0, ry + 12.0, HULL["rear_z"] + 18), (0, 1, 0), (0, 0, 1),
                  4.5, 6, shade(GREASE, 0.7))
    skin("hull", er, er2)
    cap("hull", er2, (16.0, ry + 13.0, HULL["rear_z"] + 18), (20, 18, 18))


# ---------------------------------------------------------------------------
# 2. legs — out first, down second.  This is the whole silhouette.
# ---------------------------------------------------------------------------
def build_leg(side, pair):
    """side = +1 (his left) or -1.  pair = 'front' | 'rear'.

    Built as three rigid segments rather than one continuous tube.  A tube is
    cheaper and looks better standing still, but it cannot be posed: rigid
    binding gives every vertex exactly one matrix, so a ring shared by two
    segments has to pick a parent and then tears the moment the joint bends.
    Each segment therefore carries its own copy of both end rings, and the
    child's proximal ring is oversized and pushed back into the parent so the
    wedge that opens on rotation stays buried.
    """
    nodes = leg_nodes(side, pair)
    g = leg_name(side, pair)
    grp = "legs"
    COL = [STEEL_DARK, STEEL, STEEL_LIT, GREASE]
    AO = [0.90, 0.96, 1.06, 0.72]
    NSEG = 6
    OVERLAP = 1.18      # how far the child's collar swallows the parent

    for i, bname in enumerate((g + "_hip", g + "_boom", g + "_shin")):
        a, b = nodes[i], nodes[i + 1]
        axis = sub(b, a)
        with bone(bname):
            # proximal ring, pulled back along the axis and fattened; distal
            # ring sits exactly on the child joint so the child can swallow it
            prox = ring_at(lerp3(a, b, -0.06), axis, (0, 1, 0),
                           LEG_RADII[i] * (OVERLAP if i else 1.0), NSEG,
                           COL[i], uv_v=i / 3.0, ao=AO[i])
            dist = ring_at(b, axis, (0, 1, 0), LEG_RADII[i + 1], NSEG,
                           COL[i + 1], uv_v=(i + 1) / 3.0, ao=AO[i + 1])
            skin(grp, prox, dist, flip=(side < 0))

            if i == 0:      # hip shroud where the leg enters the hull
                cap(grp, prox, (side * (HULL["half_x"] - 2), a[1], a[2]),
                    GREASE, flip=(side > 0))

            if i == 1:      # knee actuator, so the crank reads as a joint
                pa = lerp3(a, b, 0.45)
                pb = lerp3(b, nodes[3], 0.22)
                pr1 = ring_at(pa, sub(pb, pa), (0, 1, 0), 4.2, 4, GREASE)
                pr2 = ring_at(pb, sub(pb, pa), (0, 1, 0), 3.0, 4,
                              shade(STEEL_LIT, 0.9))
                skin(grp, pr1, pr2, flip=(side < 0))

    # foot: a splayed three-point pad, not a hoof.  It has to bite.
    with bone(g + "_shin"):
        ank = nodes[3]
        base = ring_at((ank[0], ank[1] - 2.0, ank[2]), (0, -1, 0), (0, 0, 1),
                       7.0, 3, shade(STEEL_DARK, 0.66),
                       phase=STANCE["toe_in"] * side)
        tips = []
        for k in range(3):
            ang = STANCE["toe_in"] * side + math.pi / 3 + k * (2 * math.pi / 3)
            tips.append(M.add((ank[0] + 14.0 * math.cos(ang), 0.0,
                               ank[2] + 14.0 * math.sin(ang)),
                              shade(CLAW, 0.52), (0.5, 1.0)))
        for i in range(3):
            j = (i + 1) % 3
            if side > 0:
                M.tri(grp, base[i], base[j], tips[i])
                M.tri(grp, base[j], tips[j], tips[i])
            else:
                M.tri(grp, base[j], base[i], tips[i])
                M.tri(grp, tips[j], base[j], tips[i])


# ---------------------------------------------------------------------------
# 3. the seam — where he ends and the machine begins
# ---------------------------------------------------------------------------
def build_graft():
    gy = TORSO["graft_y"]
    tz = TORSO["torso_z"]
    raw = GRAFT["raw"]

    # steel socket collar bolted through the pelvis
    collar_lo = ring(0, gy - 8, tz, 25.0, 20.0, 8, STEEL_LIT, ao=0.86)
    collar_hi = ring(0, gy + 3, tz, 22.0, 18.0, 8, STEEL, ao=1.0)
    skin("graft", collar_lo, collar_hi)

    # the flesh above it, discoloured and pulled tight over the rim
    seam_col = lerp3(FLESH_COLD, NECRO, raw)
    seam = ring(0, gy + 8, tz, 20.5, 16.5, 8,
                tuple(int(c) for c in seam_col), ao=0.92)
    skin("graft", collar_hi, seam)

    # staples: eight little bright quads straddling the seam
    for k in range(8):
        a = math.pi * (2 * k + 1) / 8.0
        rx, rz = 21.5, 17.5
        x, z = rx * math.cos(a), tz + rz * math.sin(a)
        ox, oz = 2.4 * -math.sin(a), 2.4 * math.cos(a)
        p0 = M.add((x - ox, gy + 1.0, z - oz), STEEL_LIT)
        p1 = M.add((x + ox, gy + 1.0, z + oz), STEEL_LIT)
        p2 = M.add((x + ox, gy + 6.5, z + oz), shade(STEEL_LIT, 0.8))
        p3 = M.add((x - ox, gy + 6.5, z - oz), shade(STEEL_LIT, 0.8))
        M.quad("graft", p0, p1, p2, p3)

    return seam


# ---------------------------------------------------------------------------
# 4. human torso, exposed spine, opened ribcage
# ---------------------------------------------------------------------------
def build_torso(seam):
    tz = TORSO["torso_z"]
    # 10 segments, not 8.  At eight the chest reads as a box with the corners
    # knocked off, and it is the largest untextured surface on the model.
    NT = 10
    waist = ring(0, TORSO["waist_y"], tz, 21.0, 15.0, NT, FLESH_COLD, ao=0.90)
    chest = ring(0, TORSO["chest_y"], tz + 1, 27.0, 17.0, NT, FLESH, ao=1.0)
    yoke  = ring(0, TORSO["yoke_y"],  tz, 25.0, 15.0, NT, FLESH_DARK, ao=0.94)

    # Segments 1 and 2 are his left front — that is the wall that comes out.
    OPEN = (1, 2) if GRAFT["open_ribs"] > 0.0 else ()

    skin("body", seam, waist)
    skin("body", waist, chest, skip=OPEN)
    skin("body", chest, yoke, skip=OPEN)
    cap("body", yoke, (0.0, TORSO["yoke_y"] + 4, tz), FLESH_DARK)

    # The ribcage, cracked open and bolted that way.
    #
    # What was here before was four flat quads standing in front of a fifth
    # flat quad — paddles, not ribs, with nothing behind them.  From any angle
    # off dead-centre they vanished to a line and the "cavity" was revealed as
    # a painted rectangle.  Ribs have to be arcs, they have to have thickness,
    # and there has to be somewhere for them to open *away from*.
    if GRAFT["open_ribs"] > 0.0:
        o = GRAFT["open_ribs"]
        cy0, cy1 = TORSO["waist_y"], TORSO["yoke_y"]

        # --- the rim of the cut.  The outer shell now has a hole in it, and a
        # hole with no thickness reads as a sticker: this walks the opening's
        # edge inward and down, so you see subcutaneous fat and then meat
        # before you see cavity.
        rim_out, rim_in = [], []
        for i in (1, 2, 3):
            for r in (waist, chest, yoke):
                p = M.v[r[i]]
                rim_out.append(M.add(p, shade(NECRO, 1.15)))
                rim_in.append(M.add((p[0] * 0.74, p[1], tz + (p[2] - tz) * 0.66),
                                    shade(CLOT, 1.2)))
        for k in range(len(rim_out) - 1):
            if (k + 1) % 3:                          # do not bridge rings
                M.quad("gore", rim_out[k], rim_in[k], rim_in[k + 1],
                       rim_out[k + 1])

        # --- the cavity itself: a concave shell set behind the opening, so the
        # hole still reads as a hole when he turns.
        NB = 5
        back, mouth = [], []
        for k in range(NB):
            t = k / (NB - 1.0)
            y = cy0 + (cy1 - cy0) * t
            wob = math.sin(t * 3.14159)
            back.append(M.add((1.0 + 2.0 * wob, y, tz - 4.0 - 4.0 * wob),
                              shade(GORE, 0.30), (0.88, t)))
            mouth.append(M.add((3.0 + 17.0 * wob, y, tz + 10.0 + 2.0 * wob),
                               shade(GORE, 0.60), (0.06, t)))
        for k in range(NB - 1):
            M.quad("gore", back[k], mouth[k], mouth[k + 1], back[k + 1])
        M.tri("gore", back[0], mouth[0], back[1])
        M.tri("gore", back[NB - 2], mouth[NB - 1], back[NB - 1])

        # --- viscera: a lung lobe gone dark, and the pump that replaced what
        # used to be beside it.  This is the whole character in one detail —
        # the machine did not stop at his waist.
        lung = []
        for k in range(4):
            t = k / 3.0
            y = cy0 + 6 + (cy1 - cy0 - 14) * t
            r = 5.0 + 3.2 * math.sin(t * 3.14159)
            lung.append(ring_at((8.0, y, tz + 1.0), (0, 1, 0), (0, 0, 1),
                                r, 5, shade(GORE, 0.8 + 0.5 * t),
                                uv_v=t, ao=0.7 + 0.4 * t))
        for k in range(3):
            skin("gore", lung[k], lung[k + 1])
        cap("gore", lung[0], (8.0, cy0 + 3, tz + 1.0), shade(CLOT, 1.0))
        cap("gore", lung[-1], (8.0, cy1 - 7, tz + 1.0), shade(GORE, 1.3),
            flip=True)

        pump_c = (16.0, (cy0 + cy1) * 0.5 - 2.0, tz + 4.0)
        pa = ring_at(pump_c, (1, 0.2, 0.7), (0, 1, 0), 4.6, 6, BRASS, ao=0.9)
        pb = ring_at(tuple(pump_c[j] + (2.4, 0.5, 1.7)[j] for j in range(3)),
                     (1, 0.2, 0.7), (0, 1, 0), 3.4, 6, STEEL_LIT, ao=1.05)
        skin("body", pa, pb)
        cap("body", pb, tuple(pump_c[j] + (4.2, 0.9, 2.9)[j] for j in range(3)),
            STEEL_LIT)
        for k in range(2):      # feed lines diving down toward the graft
            x = 12.0 + k * 4.5
            t0 = ring_at((x, cy0 + 3.0, tz + 5.0), (0.1, -1, -0.2), (0, 0, 1),
                         1.9, 4, CABLE, ao=0.7)
            t1 = ring_at((x + 1.5, TORSO["graft_y"] + 7.0, tz + 3.0),
                         (0.1, -1, -0.2), (0, 0, 1), 1.5, 4,
                         shade(CLOT, 1.1), ao=0.55)
            skin("body", t0, t1)

        # --- the ribs.  Six arcs sweeping from the spine round to the split
        # sternum, each a thin three-sided prism so it holds an edge from any
        # angle.  The roots stay put because that is where they are still
        # attached; `open_ribs` cranks the free ends out through the opening.
        NR, NS = 6, 5
        for r in range(NR):
            tr = r / (NR - 1.0)
            y_root = cy0 + (cy1 - cy0) * (0.14 + 0.74 * tr)
            span = 1.0 - 0.20 * abs(tr - 0.45) * 2.0     # ribs 3-4 are longest
            broke = (r == 4)                             # one snapped short
            ns = NS - 1 if broke else NS
            prev = None
            for sgi in range(ns):
                ts = sgi / (NS - 1.0)
                # ts = 0 is the posterior root, still attached; ts = 1 is the
                # cut sternal end.  The first pass had this backwards and swept
                # the free ends laterally, which put the whole cage on the
                # outside of his flank instead of arcing across the opening —
                # ribs wrap forward toward the midline, and a chest is opened
                # by splitting the sternum, so that is where they are cut.
                a = ts * (math.pi * 0.50)
                x = 1.0 + 22.0 * span * math.cos(a)
                z = tz - 2.0 + 17.0 * span * math.sin(a)
                y = y_root - 4.0 * ts * ts + 1.2 * math.sin(tr * 6.0)
                x += o * 9.0 * ts * ts                   # the door swinging open
                z += o * 5.0 * ts * ts
                y += o * 3.0 * ts * ts * (tr - 0.5)
                w, th = 2.0 - 0.5 * ts, 1.5
                k = 0.60 + 0.42 * ts
                # blood tracks down the outside of every rib from the root
                bone_c = lerp3(BONE, BLOOD, max(0.0, 0.70 - ts * 0.75))
                cur = [M.add((x - th, y + w, z), shade(bone_c, k * 1.12),
                             (0.5, ts)),
                       M.add((x + th, y + w * 0.4, z + th * 0.8),
                             shade(bone_c, k * 0.86), (0.5, ts)),
                       M.add((x, y - w, z - th * 0.3),
                             shade(BONE_MARROW, k * 0.72), (0.5, ts))]
                if prev:
                    for e in range(3):
                        f = (e + 1) % 3
                        M.quad("ribs", prev[e], cur[e], cur[f], prev[f])
                prev = cur
            M.tri("ribs", prev[0], prev[1], prev[2])     # raw marrow face

        # --- the retractor holding it open.  Without this it reads as damage;
        # with it, it reads as something he did to himself on purpose.
        #
        # It used to reach x=36.  The gun arm runs shoulder (24, 200, 16) to
        # elbow (32, 166, 22), so the lower jaw was sitting inside the humerus
        # and punching through it on every frame of every animation.  Kept
        # inboard of x=18 now, which is inside the cavity mouth and clear of
        # the arm through its whole swing.  check_clearance() below enforces it.
        hw_start = len(M.v)
        for k in range(2):
            y = cy0 + (cy1 - cy0) * (0.28 + 0.46 * k)
            hx, hz = 4.0 + 13.0 * o, tz + 10.0 + 3.0 * o
            bar0 = M.add((2.0, y, tz + 4.0), STEEL_LIT)
            bar1 = M.add((hx, y + 0.8, hz), STEEL_LIT)
            bar2 = M.add((hx, y - 1.4, hz), shade(STEEL_LIT, 0.7))
            bar3 = M.add((2.0, y - 2.2, tz + 4.0), shade(STEEL_LIT, 0.7))
            M.quad("body", bar0, bar1, bar2, bar3)
            # a flat clamp jaw hooked under the rib.  A capped cone here read
            # as an arrowhead, which made the retractor look like he had been
            # shot with it rather than like he had fitted it himself.
            j = [M.add((hx - 0.8, y + 2.6, hz - 0.8), STEEL_LIT),
                 M.add((hx + 2.4, y + 2.0, hz + 0.8), shade(STEEL_LIT, 0.82)),
                 M.add((hx + 2.4, y - 2.0, hz + 0.8), shade(CLAW, 0.9)),
                 M.add((hx - 0.8, y - 2.6, hz - 0.8), shade(CLAW, 1.05))]
            M.quad("body", j[0], j[1], j[2], j[3])
            M.quad("body", j[3], j[2], j[1], j[0])
        globals()["CHEST_HW"] = (hw_start, len(M.v))

    return yoke


# ---------------------------------------------------------------------------
# 5. exposed spinal column running out of his back onto the hull deck
# ---------------------------------------------------------------------------
def build_spine():
    tz = TORSO["torso_z"]
    top = (0.0, TORSO["chest_y"] + 2, tz - 16.0)
    bot = (0.0, HULL["roof_y"] - 2.0, HULL["rear_z"] + 26.0)
    n = 5
    for k in range(n):
        t0, t1 = k / n, (k + 1) / n
        p0 = lerp3(top, bot, t0)
        p1 = lerp3(top, bot, t1)
        # sag the chain so it arcs rather than runs straight
        sag = math.sin(math.pi * t0) * 6.0
        p0 = (p0[0], p0[1] - sag, p0[2])
        r = 5.0 - 1.2 * t0
        col = STEEL_LIT if k % 2 == 0 else STEEL_DARK
        a = ring_at(p0, sub(p1, p0), (0, 0, 1), r, 4, col, ao=0.9)
        b = ring_at(p1, sub(p1, p0), (0, 0, 1), r * 0.85, 4, shade(col, 0.8),
                    ao=0.9)
        skin("body", a, b)

    # two cables from the skull base down into the deck
    for sgn in (-1, 1):
        cs = (sgn * 7.0, TORSO["neck_y"] - 2, tz - 12.0)
        ce = (sgn * 12.0, HULL["roof_y"] - 2.0, HULL["rear_z"] + 40.0)
        mid = lerp3(cs, ce, 0.5)
        mid = (mid[0] + sgn * 6.0, mid[1], mid[2] - 4.0)
        for (p0, p1) in ((cs, mid), (mid, ce)):
            a = ring_at(p0, sub(p1, p0), (0, 1, 0), 2.4, 3, CABLE)
            b = ring_at(p1, sub(p1, p0), (0, 1, 0), 2.4, 3, shade(CABLE, 0.8))
            skin("body", a, b)


# ---------------------------------------------------------------------------
# 6. head — half of it is bone, half of it is plate, none of it is kind.
#
# The read has to survive 240p and a 12-pixel-wide face, so the expression is
# carried by geometry first and texture second: a bone brow that physically
# overhangs the one eye he has left, a chin tucked so he looks up through it,
# and two lenses toed in until they converge somewhere just behind your head.
# ---------------------------------------------------------------------------

# Cross-sections up the skull: (y, half-width X, half-depth Z).  Everything
# else in the head — rings, plate, staples, lens mounts — interpolates off this.
# A skull is about 15 cm across and 19 deep, and it is worth staying honest
# about that: an oversized head is the single loudest tell of a hobby model.
# (y, half-width X, half-depth Z, Z offset of the ring centre).  That last
# column is what stops it reading as a loaf: a skull throws its chin forward
# and sets its crown back, and without the offset every ring stacks on one
# axis and you get a dome on a tube.
SKULL_PROFILE = [
    (204.0,  6.2,  6.1, -1.7),   # neck root
    (211.0,  5.6,  5.9, -1.0),   # under the jaw
    (216.6,  6.0,  7.4,  0.7),   # mandible, chin thrown forward
    (222.0,  7.2,  8.6,  0.2),   # tooth line
    (227.0,  8.2,  9.2, -0.4),   # zygomatic — widest point of the skull
    (232.4,  7.9,  9.0, -1.1),   # brow
    (238.5,  6.0,  7.2, -2.3),   # crown, set back over the occiput
]
SKULL_WIDE = 8.6           # slightly proud of the widest ring; the face plate
                           # normalises against this so its corners cannot
                           # collapse to the centreline at the chin

MALICE = {
    "brow":     1.00,   # 0 = flat frontal bone, 1 = full overhanging prow
    "tuck":     1.00,   # chin drops, so the gaze comes up from under the brow
    "cant":     1.00,   # small head roll — reads as "regarding you"
    "converge": 1.00,   # lens toe-in
    "bare":     1.00,   # 0 = skin intact on his right, 1 = skull fully exposed
}

# Face-plate grid, shared with the texture painter so the two can never drift.
# Low X is his right and takes low U.  Columns are given at the widest row and
# scaled in per row, which narrows the plate at the chin the way a face does.
FACE_COLS = [-8.1, -2.9, 2.3, 7.7]
FACE_ROWS = [216.6, 221.6, 226.6, 232.4]  # chin, tooth line, aperture, brow
FACE_TEX = 64                             # 64x64 CI4 = 2048 B = all of low TMEM


def seam_x(y):
    """X of the flesh-to-steel boundary at height y.

    The cut runs on a diagonal.  The plate took his temple first and worked
    downward, so it crosses the midline high on the skull and only gives ground
    by the time it reaches the jaw.  A vertical seam looks manufactured; this
    one looks like something that spread.
    """
    t = max(0.0, min(1.0, (y - 216.0) / 22.0))
    rx, _, _ = prof_at(y)
    # As a fraction of the ring, not an absolute X.  Absolute was the bug: the
    # crown is only 6 cm across, so a seam quoted in centimetres swallowed the
    # whole top of the skull and left no bone up there at all.
    return (0.36 - 0.78 * t) * rx


def head_col(x, y, ang):
    """Material at a point on the skull: steel, seam, bone, or what skin is left."""
    b = seam_x(y)
    if x > b + 0.9:
        return STEEL if x > b + 4.5 else STEEL_DARK
    if x > b - 0.9:
        return NECRO                       # stapled, weeping, neither one thing
    # His right. Skin tore away over the brow and temple and was never replaced;
    # what is left clings to the jaw. The boundary is jittered so it reads torn.
    torn = 225.0 + 4.6 * math.sin(ang * 3.0 + 1.1) - 3.0 * MALICE["bare"]
    return BONE if y > torn else FLESH


def prof_at(y):
    """Interpolate (rx, rz, cz) off SKULL_PROFILE."""
    if y <= SKULL_PROFILE[0][0]:
        return SKULL_PROFILE[0][1:]
    for k in range(len(SKULL_PROFILE) - 1):
        a, b = SKULL_PROFILE[k], SKULL_PROFILE[k + 1]
        if y <= b[0]:
            t = (y - a[0]) / (b[0] - a[0])
            return tuple(a[i] + (b[i] - a[i]) * t for i in (1, 2, 3))
    return SKULL_PROFILE[-1][1:]


def head_tilt(p):
    """The chin-tuck and cant applied to the finished skull.

    Factored out because the jaw's hinge has to receive it too: build_bones()
    computes pivots from the untilted landmarks, so a TMJ quoted straight off
    SKULL_PROFILE ends up a couple of centimetres behind the joint it is
    supposed to be, and the jaw swings on the wrong axis.
    """
    piv = (0.0, 209.0, TORSO["torso_z"])
    pt = math.radians(8.5) * MALICE["tuck"]
    rl = math.radians(3.5) * MALICE["cant"]
    x, y, z = p
    dy, dz = y - piv[1], z - piv[2]
    y2 = piv[1] + dy * math.cos(pt) - dz * math.sin(pt)
    z2 = piv[2] + dy * math.sin(pt) + dz * math.cos(pt)
    dx, dy2 = x - piv[0], y2 - piv[1]
    return (piv[0] + dx * math.cos(rl) - dy2 * math.sin(rl),
            piv[1] + dx * math.sin(rl) + dy2 * math.cos(rl),
            z2)


TMJ = (0.0, 226.0, -3.4)     # temporomandibular joint, relative to torso_z


def skull_ring(cy, n, ao=1.0, grow=1.0):
    """A ring around the skull, each vertex coloured by which zone it lands in."""
    rx, rz, dz = prof_at(cy)
    rx, rz = rx * grow, rz * grow
    cz = TORSO["torso_z"] + dz
    out = []
    for i in range(n):
        a = 2.0 * math.pi * i / n
        x = rx * math.cos(a)
        z = cz + rz * math.sin(a)
        k = ao * (0.78 + 0.22 * max(0.0, math.sin(a)))   # front lit, back falls off
        out.append(M.add((x, cy, z), shade(head_col(x, cy, a), k), (i / n, 0.0)))
    return out


def build_optic(cx, cy, cz, r, toe, pitch, ao=1.0):
    """A recessed camera lens: housing, bezel, barrel wall, hot glass.

    The glass and the barrel go in their own group so the display list can run
    them with lighting off and PRIM colour live — that is how the lens keeps
    burning when he walks into shadow, and how you pulse it on a timer.
    """
    axis = norm((-math.sin(toe) * MALICE["converge"], -math.sin(pitch),
                 math.cos(toe)))
    c = (cx, cy, cz)

    def step(d, rad, col, grp="head", a=1.0):
        p = tuple(c[k] + axis[k] * d for k in range(3))
        return ring_at(p, axis, (0, 1, 0), rad, 8, col, ao=ao * a), p

    # Shallow on purpose.  An optic that stands off the plate reads as a lamp
    # bolted to his head; the whole point is that it was let into the bone.
    mount, _ = step(-0.8, r * 1.10, STEEL_DARK, a=0.80)   # flange against plate
    lip,   _ = step(0.35, r, STEEL_LIT)                   # bezel, barely proud
    throat, _ = step(0.05, r * 0.70, GREASE, a=0.50)      # step down into barrel
    glass, gp = step(-0.35, r * 0.66, OPTIC)              # glass sits recessed

    skin("head", mount, lip)
    skin("head", lip, throat, flip=True)
    skin("optic", throat, glass, flip=True)
    cap("optic", glass, gp, OPTIC)
    return gp


def build_head():
    tz = TORSO["torso_z"]
    head_mark = len(M.v)
    CUR_BONE.append("head")
    N = 12                                        # segments per skull ring

    rings = [skull_ring(row[0], N, ao=0.86 if row[0] < 214 else 1.0)
             for row in SKULL_PROFILE]

    # --- brow prow: the frontal bone is pulled forward and dropped over the
    # orbit, harder on the bone side than the plate side.  Symmetry reads as
    # composure, so the two halves must not agree.  Kept small on purpose — a
    # brow that clears the face plate stops being a brow and becomes a beak.
    # The plate sits in front of the skull, so a brow built on the ring would
    # simply be hidden behind it.  The ring therefore only carries the ridge
    # where it runs out past the plate — the temples — and the plate itself
    # takes the overhang a few lines further down.
    _, brz, bcz = prof_at(232.4)
    for idx in rings[5]:
        x, y, z = M.v[idx]
        front = max(0.0, (z - tz - bcz) / brz)
        if front > 0.12 and abs(x) > 5.0:
            heavy = 1.0 if x < seam_x(y) else 0.45
            M.v[idx] = (x, y - MALICE["brow"] * 1.9 * front * heavy, z)
            M.c[idx] = shade(M.c[idx], 1.0 - 0.20 * front)

    # Rows 1..3 are the mandible, and they now belong to a hinged jaw rather
    # than to the skull.  The head skips those two bands and the jaw piece
    # spans them with its own copies — rigid binding means a ring shared by
    # two bones has to pick one, and the one it picks tears.  The jaw's collar
    # rings are 6% oversized so the wedge that opens at each end when he talks
    # stays inside the skull instead of showing daylight.
    for k, (a, b) in enumerate(zip(rings, rings[1:])):
        if k in (1, 2):
            continue
        skin("head", a, b)

    with bone("jaw"):
        j_top = skull_ring(SKULL_PROFILE[3][0], N, grow=1.06)
        j_mid = skull_ring(SKULL_PROFILE[2][0], N)
        j_low = skull_ring(SKULL_PROFILE[1][0], N, grow=1.06, ao=0.86)
        skin("head", j_top, j_mid)
        skin("head", j_mid, j_low)
    cap("head", rings[-1], (0.0, 241.0, tz - 3.4), shade(STEEL_LIT, 0.88))

    # --- face plate.  A 3x3 grid rather than the usual single quad: it lets the
    # panel wrap the cheeks, sit under the brow instead of in front of it, and
    # carry a real recess where the nose is not.  Z comes off the same profile
    # the rings use, so it rides the skull instead of floating in front of it.
    def plate_v(ci, ri):
        y = FACE_ROWS[ri]
        rx, rz, dz = prof_at(y)
        x = FACE_COLS[ci] * (rx / SKULL_WIDE)
        # 2.6 cm of standoff, not 1.15.  The plate has to beat the skull ring
        # at every row or the ring wins the depth test and you get a blank bone
        # plane where the face should be — which is exactly what happened, and
        # the aperture recess below made it worse by sinking the middle of the
        # plate a full centimetre behind the frontal bone.
        z = (tz + dz
             + rz * math.sqrt(max(0.0, 1.0 - (x / SKULL_WIDE) ** 2)) + 2.6)
        if -6.0 < x < 0.8 and 223.0 < y < 229.0:  # piriform aperture: no nose,
            z -= 1.5                              # just a hole where one was
        u = 0.02 + 0.96 * ci / 3.0
        v = 0.02 + 0.96 * (3 - ri) / 3.0
        # Bake the brow's shadow into the plate rather than trusting the texture
        # to carry it: the row under the ridge darkens, and darkens further on
        # the bone side, where the ridge actually overhangs.
        k = 1.0
        if ri == 2:
            k = 0.62 if FACE_COLS[ci] < 0 else 0.82
        elif ri == 3:
            k = 0.74
        c = int(round(255 * k))
        return M.add((x, y, z), (c, c, c), (u, v))

    grid = [[plate_v(c, r) for c in range(4)] for r in range(4)]

    # the prow, applied to the plate's top row.  Forward and down, and down
    # harder on the bone side, because symmetry reads as composure.
    for ci, idx in enumerate(grid[3]):
        x, y, z = M.v[idx]
        heavy = 1.0 if x < seam_x(y) else 0.5
        inboard = 1.0 - abs(FACE_COLS[ci]) / FACE_COLS[-1] * 0.45
        M.v[idx] = (x,
                    y - MALICE["brow"] * 2.4 * heavy * inboard,
                    z + MALICE["brow"] * 1.5 * inboard)

    # The lowest band of the plate carries the mouth, so it goes on the jaw.
    # Caveat worth knowing: that band spans texels 42-63, which is both tooth
    # arches, so the maxillary teeth travel with the mandible.  At 64x64 and a
    # 10-degree opening it reads; if it ever stops reading, the fix is a fifth
    # row in FACE_ROWS splitting the arches, not a smaller jaw angle.
    for r in range(3):
        for c in range(3):
            if r == 0:
                with bone("jaw"):
                    q = [M.add(M.v[grid[rr][cc]], M.c[grid[rr][cc]],
                               M.uv[grid[rr][cc]])
                         for (rr, cc) in ((0, c), (0, c + 1), (1, c + 1), (1, c))]
                M.quad("face", q[0], q[1], q[2], q[3])
            else:
                M.quad("face", grid[r][c], grid[r][c + 1],
                       grid[r + 1][c + 1], grid[r + 1][c])

    # --- the two lenses.  Primary sits where his left eye was; the spotter is
    # bolted high on the temple and cants down.  Both toe inward, so their axes
    # cross in front of the face — convergence is what makes it read as aiming
    # rather than as decoration.
    # Positions are read back out of the texture rather than guessed, so the
    # bezel rings land on the painted housings instead of floating beside them.
    def mount_z(x, y, out=0.0):
        rx, rz, dz = prof_at(y)
        return (tz + dz
                + rz * math.sqrt(max(0.0, 1.0 - (x / SKULL_WIDE) ** 2))
                + 2.6 + out)

    def from_tex(tx, ty, rpx):
        """Texel centre and radius -> plate-space centre and radius."""
        u, v = tx / (FACE_TEX - 1.0), ty / (FACE_TEX - 1.0)
        ci = (u - 0.02) * 3.0 / 0.96
        ri = 3.0 - (v - 0.02) * 3.0 / 0.96
        cx = FACE_COLS[int(ci)] + (ci % 1.0) * (FACE_COLS[min(3, int(ci) + 1)]
                                                - FACE_COLS[int(ci)])
        cy = FACE_ROWS[int(ri)] + (ri % 1.0) * (FACE_ROWS[min(3, int(ri) + 1)]
                                                - FACE_ROWS[int(ri)])
        span = (FACE_COLS[-1] - FACE_COLS[0]) / (FACE_TEX - 1.0)
        rx, _, _ = prof_at(cy)
        return cx * (rx / SKULL_WIDE), cy, rpx * span

    # These four numbers are the texel coordinates the painter uses for the two
    # sockets.  Keep them here and in build_textures in step, or the bezels
    # land next to the painted housings instead of on them.
    px, py, pr = from_tex(48.0, 18.0, 8.0)
    build_optic(px, py, mount_z(px, py, 0.1), pr,
                math.radians(15.0), math.radians(2.0))
    sx, sy, sr = from_tex(57.0, 39.0, 4.8)
    build_optic(sx, sy, mount_z(sx, sy, -0.1), sr,
                math.radians(26.0), math.radians(6.0))

    # --- staples.  Most of the suturing lives in the texture, but the seam runs
    # off the front of the face and around the skull, and texture cannot follow
    # it there.  These six carry the silhouette.
    for y in (218.5, 222.5, 226.0, 229.5, 232.5, 235.5):
        b = seam_x(y)
        rx, rz, dz = prof_at(y)
        f = max(0.0, 1.0 - (b / rx) ** 2)
        z = tz + dz + rz * math.sqrt(f) + 0.35
        w, h = 2.9, 0.85
        p = [M.add((b - w, y - h, z - 0.9), shade(STEEL_LIT, 0.85)),
             M.add((b + w, y - h, z - 0.9), STEEL_LIT),
             M.add((b + w, y + h, z), STEEL_LIT),
             M.add((b - w, y + h, z), shade(STEEL_LIT, 0.85))]
        M.quad("head", p[0], p[1], p[2], p[3])

    # --- attitude.  Pitch the whole head about the atlas so the chin tucks and
    # the brow comes down between him and you, then cant it a few degrees.  The
    # eye is painted high in its socket to compensate, so the gaze stays level
    # while the skull does not.
    piv = (0.0, 209.0, tz)
    pt = math.radians(8.5) * MALICE["tuck"]
    rl = math.radians(3.5) * MALICE["cant"]
    for i in range(head_mark, len(M.v)):
        x, y, z = M.v[i]
        dy, dz = y - piv[1], z - piv[2]
        y2 = piv[1] + dy * math.cos(pt) - dz * math.sin(pt)
        z2 = piv[2] + dy * math.sin(pt) + dz * math.cos(pt)
        dx, dy2 = x - piv[0], y2 - piv[1]
        M.v[i] = (piv[0] + dx * math.cos(rl) - dy2 * math.sin(rl),
                  piv[1] + dx * math.sin(rl) + dy2 * math.cos(rl),
                  z2)
    CUR_BONE.pop()


# ---------------------------------------------------------------------------
# 7. right arm — still his, mostly.  Fingers fused into a working mitt.
# ---------------------------------------------------------------------------
def build_right_arm():
    tz = TORSO["torso_z"]
    sh = (-24.0, TORSO["yoke_y"] - 2, tz)
    el = (-30.0, TORSO["chest_y"] - 22, tz + 4)
    wr = (-31.0, TORSO["waist_y"] - 12, tz + 10)

    # The elbow already gets two rings, so the arm splits into two rigid limbs
    # for free — no extra geometry needed to keep the joint closed.
    with bone("armR"):
        a = ring_at(sh, sub(el, sh), (0, 0, 1), 8.5, 6, FLESH_DARK, ao=0.9)
        b = ring_at(el, sub(el, sh), (0, 0, 1), 6.5, 6, FLESH_COLD, ao=0.95)
        skin("body", a, b, flip=True)
        cap("body", a, sh, FLESH_DARK, flip=True)

        c = ring_at(el, sub(wr, el), (0, 0, 1), 6.9, 6, FLESH_COLD, ao=0.95)
        d = ring_at(wr, sub(wr, el), (0, 0, 1), 5.0, 6, STEEL, ao=0.9)
        skin("body", c, d, flip=True)

        # mitt: two fused finger blocks, plain steel.  Not a hand any more, but
        # still the one part of him that can hold something.
        for k in range(2):
            x = wr[0] - 2.6 + k * 5.2
            box("body", (x - 2.3, wr[1] - 13.0, wr[2] - 3.2),
                (x + 2.3, wr[1] - 1.0, wr[2] + 3.2),
                STEEL_LIT if k == 0 else STEEL)


# ---------------------------------------------------------------------------
# 8. left arm — the gun.  Break-action double barrel, three claws round it.
# ---------------------------------------------------------------------------
def build_shotgun_arm():
    tz = TORSO["torso_z"]
    sh = (24.0, TORSO["yoke_y"] - 2, tz)
    el = (32.0, TORSO["chest_y"] - 20, tz + 6)
    mz = (38.0, TORSO["waist_y"] - 26, tz + 34)     # muzzle plane

    # upper arm: flesh, but wasted — the gun does the work now
    with bone("armL"):
        a = ring_at(sh, sub(el, sh), (0, 0, 1), 8.5, 6, FLESH_DARK, ao=0.9)
        b = ring_at(el, sub(el, sh), (0, 0, 1), 6.0, 6, FLESH_COLD, ao=0.88)
        skin("gun", a, b)
        cap("gun", a, sh, FLESH_DARK)

    with bone("gun"):
        axis = sub(mz, el)
        ax = norm(axis)
        side = norm(cross((0.0, 1.0, 0.0), ax))
        up = cross(ax, side)

        # receiver: a brass block bolted straight onto the humerus
        r0 = ring_at(el, axis, (0, 1, 0), 8.0, 6, NECRO, ao=0.9)
        r1 = ring_at(lerp3(el, mz, 0.16), axis, (0, 1, 0), 9.5, 6, BRASS, ao=1.0)
        r2 = ring_at(lerp3(el, mz, 0.42), axis, (0, 1, 0), 9.0, 6, BRASS, ao=0.94)
        skin("gun", r0, r1)
        skin("gun", r1, r2)

        # hinge pin and break lever, so it reads as a real break-action
        hp = lerp3(el, mz, 0.30)
        sgn = 1
        p = tuple(hp[k] + sgn * 9.0 * side[k] for k in range(3))
        h0 = ring_at(p, side, ax, 3.0, 4, STEEL_LIT)
        h1 = ring_at(tuple(p[k] + sgn * 3.0 * side[k] for k in range(3)),
                     side, ax, 3.0, 4, shade(STEEL_LIT, 0.8))
        skin("gun", h0, h1)
        cap("gun", h1, tuple(p[k] + sgn * 4.0 * side[k] for k in range(3)),
            STEEL_DARK)

        # the two barrels, side by side
        bstart = lerp3(el, mz, 0.42)
        for sgn in (-1, 1):
            off = tuple(sgn * 4.6 * side[k] for k in range(3))
            p0 = tuple(bstart[k] + off[k] for k in range(3))
            p1 = tuple(mz[k] + off[k] for k in range(3))
            b0 = ring_at(p0, axis, (0, 1, 0), 4.4, 6, GUNMETAL, ao=0.9)
            b1 = ring_at(p1, axis, (0, 1, 0), 4.2, 6, shade(GUNMETAL, 1.1), ao=1.0)
            skin("gun", b0, b1)
            # bore: dark ring closing the muzzle
            cap("gun", b1, tuple(p1[k] + 0.4 * ax[k] for k in range(3)), (14, 12, 14))

        # three claws bolted around the muzzle where the hand used to be.
        # They splay outward and curl forward past the bores.  Each one gets
        # its own bone hinged at the knuckle, so the muzzle can open into a
        # grab and clamp shut on something — a claw welded rigid to the barrel
        # can only ever be scenery.
        for k in range(3):
            ang = math.pi / 2 + k * (2 * math.pi / 3)
            radial = tuple(math.cos(ang) * side[j] + math.sin(ang) * up[j]
                           for j in range(3))
            knuckle = tuple(mz[j] - 5.0 * ax[j] + 8.5 * radial[j] for j in range(3))
            mid     = tuple(mz[j] + 9.0 * ax[j] + 11.0 * radial[j] for j in range(3))
            tip     = tuple(mz[j] + 24.0 * ax[j] + 5.0 * radial[j] for j in range(3))

            with bone("claw%d" % k):
                c0 = ring_at(knuckle, sub(mid, knuckle), up, 3.4, 3,
                             STEEL_DARK, ao=0.9)
                c1 = ring_at(mid, sub(tip, mid), up, 2.6, 3, CLAW, ao=1.0)
                skin("gun", c0, c1)
                ti = M.add(tip, shade(CLAW, 1.06), (0.5, 1.0))
                for i in range(3):
                    j = (i + 1) % 3
                    M.tri("gun", c1[i], c1[j], ti)
                cap("gun", c0,
                    tuple(knuckle[j] - 2.0 * ax[j] for j in range(3)),
                    STEEL_DARK, flip=True)


# ---------------------------------------------------------------------------
# One palette for every texture on the character.
#
# The face and the hull plate used to be quantised separately — a fixed 16 for
# the face, an adaptive 16 for the plate — and they did not match: the plate's
# greys landed a few points cooler than the face's, so the two read as parts
# from different models wherever they met at the graft.  Sharing one palette
# fixes that, and it pays for itself on hardware as well: CI4 TLUTs live in the
# top half of TMEM, so one shared TLUT means one load for all three textures
# instead of a reload every time the display list changes material.
#
# Sixteen slots, and the blood does double duty.  CLOT -> BLOOD -> OPTIC is the
# lens ramp as well as the wound ramp, which is not a compromise — the lens
# glow and what is running down his chest ought to be the same red.  The iris
# takes STEEL, because a dead grey eye is worse than a coloured one.
# ---------------------------------------------------------------------------
P_VOID, P_CLOT, P_BLOOD, P_BLOOD_L = 0, 1, 2, 3
P_GORE, P_NECRO, P_FLESH_D, P_FLESH = 4, 5, 6, 7
P_BONE_D, P_BONE, P_TEETH, P_STEEL_D = 8, 9, 10, 11
P_STEEL, P_STEEL_L, P_RUST, P_OPT = 12, 13, 14, 15

SHARED_PAL = [
    (10, 8, 10),        # VOID     — cavities, sockets, the inside of the mouth
    (44, 12, 16),       # CLOT     — old blood, gone black at the edges
    (108, 20, 22),      # BLOOD    — wet, and still arriving
    (168, 42, 38),      # BLOOD_L  — arterial, only where it is fresh
    (86, 40, 40),       # GORE     — meat, viscera, the pleura
    NECRO,              # NECRO    — dead flesh at every seam
    FLESH_DARK,
    FLESH,
    (146, 138, 118),    # BONE_D
    BONE,
    (234, 228, 208),    # TEETH    — doubles as every specular pip
    STEEL_DARK,
    STEEL,
    (168, 172, 178),    # STEEL_L
    IRON_OX,            # RUST     — and the transition out of dried blood
    (206, 44, 36),      # OPTIC    — lens core
]
PAL_FLAT = [c for rgb in SHARED_PAL for c in rgb] + [0] * (768 - 48)


def new_ci(w, h, fill):
    """A blank indexed image on the shared palette."""
    from PIL import Image
    im = Image.new("P", (w, h), fill)
    im.putpalette(PAL_FLAT)
    return im


def painter(im, w, h):
    """rect / disc / spatter helpers bound to one indexed image."""
    px = im.load()

    def rect(x0, y0, x1, y1, c):
        for y in range(max(0, int(y0)), min(h, int(y1))):
            for x in range(max(0, int(x0)), min(w, int(x1))):
                px[x, y] = c

    def disc(cx, cy, r0, r1, c):
        """Filled disc or annulus, r0 <= d < r1."""
        for y in range(max(0, int(cy - r1) - 1), min(h, int(cy + r1) + 2)):
            for x in range(max(0, int(cx - r1) - 1), min(w, int(cx + r1) + 2)):
                d = math.hypot(x - cx, y - cy)
                if r0 <= d < r1:
                    px[x, y] = c

    def run(x, y, length, c, drift=0.0):
        """A rivulet.  Blood does not fall in straight lines on a moving
        machine — it tracks, catches on a weld, and goes again."""
        fx = float(x)
        for k in range(int(length)):
            fx += drift + 0.42 * math.sin(k * 0.9 + x)
            yy = y + k
            if 0 <= yy < h and 0 <= int(fx) < w:
                px[int(fx), yy] = c
                if k > length * 0.55 and 0 <= int(fx) + 1 < w:
                    px[int(fx) + 1, yy] = P_CLOT

    def spatter(cx, cy, r, c, n, seed=1):
        """Deterministic, because a texture that changes between builds is a
        texture you cannot diff."""
        s = seed
        for _ in range(n):
            s = (s * 1103515245 + 12345) & 0x7FFFFFFF
            a = (s % 628) / 100.0
            s = (s * 1103515245 + 12345) & 0x7FFFFFFF
            d = r * ((s % 100) / 100.0) ** 0.5
            x, y = int(cx + d * math.cos(a)), int(cy + d * math.sin(a))
            if 0 <= x < w and 0 <= y < h:
                px[x, y] = c
    return px, rect, disc, run, spatter


# ---------------------------------------------------------------------------
# textures
# ---------------------------------------------------------------------------
def build_textures(outdir):
    from PIL import Image

    # ---- face: bone and what skin is left on his right, plate on his left ---
    # Painted straight into the indexed image rather than quantised down from
    # RGB.  Adaptive quantisation kept merging the pupil into the socket and the
    # hot core of the lens into the glass, which are the two colours the whole
    # face depends on.
    P_OPT_D, P_OPT_HOT, P_IRIS = P_CLOT, P_BLOOD_L, P_STEEL

    T = FACE_TEX
    face = new_ci(T, T, P_BONE)
    px, rect, disc, run, spatter = painter(face, T, T)

    # --- the seam, derived from the same functions the mesh uses -----------
    # Inverting the plate's own UV mapping rather than assuming a linear one:
    # the columns narrow per row, so a straight inverse puts the staples a few
    # texels off the geometry and the seam stops lining up with the silhouette.
    def lerp_tab(tab, f):
        f = max(0.0, min(len(tab) - 1.0001, f))
        i = int(f)
        return tab[i] + (tab[i + 1] - tab[i]) * (f - i)

    def inv_tab(tab, val):
        for i in range(len(tab) - 1):
            if (tab[i] - val) * (tab[i + 1] - val) <= 0:
                d = tab[i + 1] - tab[i]
                return i + ((val - tab[i]) / d if abs(d) > 1e-9 else 0.0)
        return 0.0 if val < tab[0] else len(tab) - 1.0

    def seam_px(row):
        v = row / (T - 1.0)
        ri = 3.0 - (v - 0.02) / 0.96 * 3.0
        y = lerp_tab(FACE_ROWS, ri)
        rx, _, _ = prof_at(y)
        xs = seam_x(y) / (rx / SKULL_WIDE)      # undo the per-row narrowing
        ci = inv_tab(FACE_COLS, xs)
        return (0.02 + 0.96 * ci / 3.0) * (T - 1.0)

    SEAM = [seam_px(r) + 1.2 * math.sin(r * 0.55) for r in range(T)]

    # --- base zones ---------------------------------------------------------
    for y in range(T):
        s = SEAM[y]
        for x in range(T):
            if x > s + 1.4:
                n = ((x * 7 + y * 13) % 19) / 19.0
                px[x, y] = P_STEEL_D if n < 0.10 else P_STEEL
            elif x > s - 1.4:
                px[x, y] = P_NECRO
            else:
                # his right.  Skin survives only on the outer cheek and jaw;
                # everything inboard of it is bare bone.
                edge = 11.0 + 2.4 * math.sin(y * 0.42)
                if y > 32 and x < edge:
                    px[x, y] = P_FLESH if x > 2 else P_FLESH_D
                else:
                    px[x, y] = P_BONE_D if x < 4 else P_BONE

    for y in range(33, T):                           # torn edge of that skin
        e = int(11.0 + 2.4 * math.sin(y * 0.42))
        rect(e - 1, y, e + 1, y + 1, P_NECRO)

    # --- his left: the plate ------------------------------------------------
    for y in (0, 1, 30, 31, 51, 52):                 # plate joins
        rect(SEAM[y] + 2, y, T, y + 1, P_STEEL_D)
    for y in (2, 3):                                 # top bevel catches light
        rect(SEAM[y] + 2, y, T, y + 1, P_STEEL_L)
    rect(38, 53, T, T, P_STEEL_D)                    # mandible plate
    disc(56, 58, 0, 4, P_STEEL)                      # jaw hinge boss
    disc(56, 58, 0, 2, P_STEEL_L)
    for (rx_, ry_) in ((37, 8), (61, 8), (61, 34), (37, 47), (60, 47)):
        px[rx_, ry_] = P_STEEL_L
        px[min(T - 1, rx_ + 1), min(T - 1, ry_ + 1)] = P_STEEL_D
    # Rust weeping off the seam.  Two columns, not three, and only where the
    # seam is not already carrying a staple — three overlapping dither loops
    # turned the whole midline into static, which at 12 pixels wide is the one
    # thing that reads worse than a blank strip.
    for col in (2, 4):
        for y in range(12, T):
            x = int(SEAM[y]) + col
            if 0 <= x < T and (y + col) % 5 == 0:
                px[x, y] = P_RUST

    # --- the two lenses.  Both sockets are painted in full, so the face still
    # reads once the geometry barrels drop at distance.  Primary sits where his
    # left eye was; the spotter is set outboard on the cheek plate and lower,
    # so the pair makes a diagonal instead of a symmetrical stare.
    def lens(cx, cy, r, hot=True):
        disc(cx, cy, r * 0.80, r, P_STEEL_D)         # housing
        disc(cx, cy, r * 0.66, r * 0.80, P_STEEL_L)  # bezel
        disc(cx, cy, r * 0.46, r * 0.66, P_VOID)     # barrel, seen end-on
        disc(cx, cy, r * 0.30, r * 0.46, P_OPT_D)
        disc(cx, cy, r * 0.14, r * 0.30, P_OPT)
        disc(cx, cy, 0.0, r * 0.14, P_OPT_HOT if hot else P_OPT)
        px[int(cx - r * 0.22), int(cy - r * 0.22)] = P_TEETH   # cover-glass pip

    LENS_MAIN = (48.0, 18.0, 10.0)
    LENS_SPOT = (57.0, 39.0, 6.0)
    lens(*LENS_MAIN)
    lens(*LENS_SPOT)

    # a flap of his own face, dragged across the midline and stapled down onto
    # the steel.  This is the detail that says what was done to him: not lost,
    # salvaged.  Solid patch, one outlined edge, four staples — any busier and
    # it turns to noise at 12 pixels wide.
    for y in range(38, 57):
        s = int(SEAM[y]) + 2
        w = int(10 - 5 * abs(y - 47) / 10.0)
        rect(s, y, s + w, y + 1, P_FLESH if (y % 9) else P_FLESH_D)
        px[min(T - 1, s + w), y] = P_NECRO
    for y in (40, 45, 50, 55):
        s = int(SEAM[y]) + 2
        w = int(10 - 5 * abs(y - 47) / 10.0)
        rect(s + w - 3, y, s + w + 2, y + 1, P_STEEL_L)
        px[max(0, s + w - 4), y] = P_VOID
        px[min(T - 1, s + w + 2), y] = P_VOID

    # --- the brow.  Everything else is decoration; this is the expression. ---
    # A bone ridge whose underside descends toward the midline, so the shadow
    # over the eye is deepest at the inner corner.  Angry is a diagonal.
    for x in range(0, int(SEAM[8]) + 1):
        low = 8 + 0.34 * x
        rect(x, 0, x + 1, low, P_BONE_D)
        rect(x, 1, x + 1, 4, P_BONE)                 # lit crest of the ridge
        rect(x, low - 3, x + 1, low, P_VOID)         # hard shadow beneath it

    # --- the eye.  Lidless: the skin over the orbit is gone, so there is no
    # lid left to soften it.  The iris rides high in the sclera, which — with
    # the head tucked — puts his gaze level with yours and the whites below it.
    #
    # Drawn as an angular orbit, not a disc.  A circle is the one shape a real
    # orbit is not, and at this size a circle full of white reads as a cartoon
    # googly eye however carefully the iris inside it is placed.  The socket is
    # a rounded trapezoid: top edge dropping toward the midline under the brow,
    # sharp inferolateral corner, wider than it is tall.
    def poly(pts, c):
        ys = [p[1] for p in pts]
        for y in range(max(0, int(min(ys))), min(T, int(max(ys)) + 1)):
            xs = []
            for i in range(len(pts)):
                (ax, ay), (bx, by) = pts[i], pts[(i + 1) % len(pts)]
                if (ay <= y < by) or (by <= y < ay):
                    xs.append(ax + (bx - ax) * (y - ay) / (by - ay))
            xs.sort()
            for k in range(0, len(xs) - 1, 2):
                rect(xs[k], y, xs[k + 1] + 1, y + 1, c)

    def ell(cx, cy, rx_, ry_, c):
        for y in range(max(0, int(cy - ry_)), min(T, int(cy + ry_) + 1)):
            dy = (y - cy) / ry_
            if abs(dy) > 1.0:
                continue
            half = rx_ * math.sqrt(1.0 - dy * dy)
            rect(cx - half, y, cx + half + 1, y + 1, c)

    ORBIT = [(7, 16), (14, 12), (23, 15), (25, 22), (21, 29), (11, 28), (6, 23)]
    poly(ORBIT, P_BONE_D)                            # orbital rim, in bone
    poly([(x + (1 if x < 16 else -1), y + (1 if y < 21 else -1))
          for (x, y) in ORBIT], P_VOID)              # the socket itself, unlit

    ell(15.5, 20.5, 5.4, 3.9, P_BONE)                # eyeball, almond not disc
    ell(15.5, 19.0, 2.9, 2.6, P_IRIS)                # iris, parked high
    ell(15.5, 18.6, 1.3, 1.2, P_VOID)                # pupil, small — he is lit
    px[14, 17] = P_TEETH                             # catchlight
    rect(10, 22, 21, 23, P_NECRO)                    # inferior rim, tight
    for (vx, vy) in ((11, 21), (20, 21), (12, 18)):  # burst vessels
        px[vx, vy] = P_NECRO

    # --- zygomatic arch, and the hollow under it ----------------------------
    for x in range(2, 18):
        y = int(31 + 0.36 * (x - 2))
        rect(x, y, x + 1, y + 1, P_BONE)
        rect(x, y + 1, x + 1, y + 4, P_BONE_D)

    # --- no nose.  A piriform aperture, sitting clear of the seam. ----------
    for y in range(28, 43):
        t = (y - 28) / 15.0
        w = 1.4 + 4.0 * t
        rect(26 - w, y, 27 + w * 0.4, y + 1, P_VOID)
    rect(21, 41, 29, 43, P_BONE_D)                   # the sill below it

    # --- teeth.  No lips on this side, so it is not a smile; it is only what
    # a face looks like without one.
    def tooth_row(y0, y1, x0, gaps, short):
        """Irregular by design.  Even spacing and a uniform length read as a
        comb; the whole point of a bared arch is that it is not tidy."""
        for y in range(y0, y1):
            for x in range(x0, T):
                if x < SEAM[y] - 1:
                    px[x, y] = P_TEETH
        n, x = 0, x0 + 1
        while x < int(SEAM[y0]) - 1:
            w = 2 + (n % 3 == 1)                     # some teeth wider
            rect(x, y0, x + 1, y1, P_BONE_D)         # the gap beside it
            if n in gaps:                            # and some simply missing
                rect(x, y0, x + w + 1, y1, P_VOID)
            elif n in short:                         # or broken off short
                rect(x + 1, y1 - 3, x + w + 1, y1, P_BONE_D)
            x += w + 1
            n += 1
        rect(x0, y0, T, y0 + 1, P_BONE_D)            # alveolar margin

    tooth_row(45, 53, 3, (2, 7), (4,))               # maxilla
    for y in range(53, 56):                          # the gap between the jaws
        rect(2, y, int(SEAM[y]) - 1, y + 1, P_VOID)
    tooth_row(56, 62, 4, (5,), (1, 6))               # mandible
    rect(2, 62, 30, T, P_BONE_D)
    rect(0, 57, 3, T, P_FLESH_D)                     # last skin at the jawline

    # --- staples down the seam ----------------------------------------------
    for y in range(4, T - 3, 6):
        s = int(SEAM[y])
        rect(s - 2, y, s + 3, y + 1, P_STEEL_L)
        px[max(0, s - 3), y] = P_VOID
        px[min(T - 1, s + 3), y] = P_VOID
        rect(s - 2, y + 1, s + 3, y + 2, P_GORE)     # shadow, and what weeps

    face_q = face
    face_q.save(os.path.join(outdir, "mc_face.png"))

    # ---- hull plate: riveted, oxidised, and running -----------------------
    # Painted on the shared palette instead of being quantised from RGB, so its
    # greys are the same greys as the face.  He rides this deck with an open
    # chest, so the top of it is where everything lands.
    plate = new_ci(32, 32, P_STEEL)
    pp, prect, pdisc, prun, pspat = painter(plate, 32, 32)
    for y in range(32):
        for x in range(32):
            if (x * 7 + y * 13) % 11 < 3:
                pp[x, y] = P_STEEL_D
    for y in (0, 1, 16, 17, 31):                    # plate joins
        prect(0, y, 32, y + 1, P_STEEL_D)
    for x in (0, 15, 16, 31):
        prect(x, 0, x + 1, 32, P_STEEL_D)
    for (rx, ry) in ((4, 4), (12, 4), (20, 4), (28, 4),
                     (4, 20), (12, 20), (20, 20), (28, 20)):
        pp[rx, ry] = P_STEEL_L
        pp[(rx + 1) % 32, ry] = P_STEEL_D
        pp[rx, (ry + 1) % 32] = P_STEEL_D
    for x in (6, 22, 24):                           # rust weeping from welds
        for y in range(2, 30):
            if (x * 3 + y) % 5:
                pp[x, y] = P_RUST
    # and what comes off him: a pool by the graft socket, dried at the rim,
    # with runs tracking down toward the join
    pdisc(11, 9, 0, 5.4, P_BLOOD)
    pdisc(11, 9, 4.0, 6.4, P_CLOT)
    pdisc(10, 8, 0, 2.2, P_BLOOD_L)
    pspat(11, 9, 11, P_CLOT, 26, seed=7)
    pspat(20, 22, 9, P_CLOT, 14, seed=19)
    for (sx, sy, ln) in ((9, 14, 15), (13, 13, 17), (21, 24, 7)):
        prun(sx, sy, ln, P_BLOOD, drift=0.05)
    plate.save(os.path.join(outdir, "mc_plate.png"))

    # ---- viscera: the inside of the thoracic cavity ------------------------
    # Wet, layered, and deliberately low-contrast except at the highlights —
    # the shape reads from the geometry, the texture only has to say "this is
    # not upholstery".
    gore = new_ci(32, 32, P_GORE)
    gp, grect, gdisc, grun, gspat = painter(gore, 32, 32)
    for y in range(32):                              # pleural sheen, banded
        for x in range(32):
            n = (x * 5 + y * 11) % 13
            if n < 3:
                gp[x, y] = P_CLOT
            elif n > 10:
                gp[x, y] = P_BLOOD
    for cy in (6, 17, 27):                           # lobes
        gdisc(9 + (cy % 7), cy, 0, 7.5, P_BLOOD)
        gdisc(9 + (cy % 7), cy, 5.5, 8.0, P_CLOT)
        gdisc(8 + (cy % 7), cy - 2, 0, 2.4, P_BLOOD_L)
        gp[7 + (cy % 7), cy - 3] = P_TEETH           # wet highlight
    for x in range(2, 30, 6):                        # vessels
        grun(x, 1, 30, P_CLOT, drift=0.12)
    grect(0, 27, 32, 32, P_CLOT)                     # pooled at the bottom
    gspat(16, 29, 14, P_BLOOD, 30, seed=3)
    gore.save(os.path.join(outdir, "mc_gore.png"))

    return face_q, plate, gore


def ci4_blob(img_p, name):
    """Emit a CI4 pixel array + RGBA5551 TLUT as C source."""
    pal = img_p.getpalette()[:48]
    idx = list(img_p.getdata())
    w, h = img_p.size

    tlut = []
    for i in range(16):
        r, g, b = pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]
        v = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1
        tlut.append(v)

    packed = []
    for y in range(h):
        row = idx[y * w:(y + 1) * w]
        for x in range(0, w, 2):
            packed.append(((row[x] & 0xF) << 4) | (row[x + 1] & 0xF))

    out = []
    out.append("static const unsigned short %s_tlut[16] __attribute__((aligned(8))) = {" % name)
    out.append("    " + ", ".join("0x%04X" % v for v in tlut))
    out.append("};")
    out.append("static const unsigned char %s_ci4[%d] __attribute__((aligned(8))) = {" % (name, len(packed)))
    for i in range(0, len(packed), 16):
        out.append("    " + ", ".join("0x%02X" % v for v in packed[i:i + 16]) + ",")
    out.append("};")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# animation.  Keyframes are authored here, in degrees and centimetres, and are
# the single source for both the C tracks and the preview renderer — so what
# the GIF shows is what the console plays, not a separate hand-tuned mock-up.
#
# A track is: bone -> [(frame, rx, ry, rz, tx, ty, tz), ...].  Rotations are
# applied about the bone's own pivot in XYZ order and are linearly interpolated
# between keys; translation is in centimetres and only the root and the gun
# actually use it.  Anything a track does not mention stays at rest.
# ---------------------------------------------------------------------------
def legs_pass(tracks, keys, front=1.0, rear=1.0, mirror=True, overwrite=False):
    """Drive all four legs from one set of shape keys.

    Most animations only ever touched the two front hips, so from the knees
    down he was a tripod with a mime on top: the arm did everything and 224
    triangles of chassis sat perfectly still through a shotgun blast.  This
    applies a shape to every leg, scaled per pair — front legs brace, rear legs
    drive — and signs the roll by side the way the gait does.

    keys = [(frame, yaw, roll, knee), ...].  `mirror` flips the yaw on his
    right so a shape that steps forward steps forward on both sides; turn it
    off for shapes that should sweep the same way across the whole body.
    """
    for side in (1, -1):
        for pair in ("front", "rear"):
            g = leg_name(side, pair)
            amp = front if pair == "front" else rear
            ms = side if mirror else 1.0
            hip = [(f, 0.0, -yaw * amp * ms, roll * amp * side, 0, 0, 0)
                   for (f, yaw, roll, knee) in keys]
            boom = [(f, 0.0, -yaw * 0.3 * amp * ms, -roll * 0.25 * amp * side,
                     0, 0, 0) for (f, yaw, roll, knee) in keys]
            shin = [(f, 0.0, 0.0, knee * amp * side, 0, 0, 0)
                    for (f, yaw, roll, knee) in keys]
            for suffix, track in (("_hip", hip), ("_boom", boom),
                                  ("_shin", shin)):
                if overwrite or (g + suffix) not in tracks:
                    tracks[g + suffix] = track


def build_anims():
    A = {}

    # --- idle.  He does not stand still; he settles.  The chassis takes the
    # weight in slow hydraulic cycles, the human half rides it a beat late,
    # and the head sweeps because the lenses are looking for you.
    idle = {}
    F = 96
    idle["hull"] = [(0, 0, 0, 0, 0, 0.0, 0), (24, 0.5, 0, 0.4, 0, -0.7, 0),
                    (48, 0, 0, 0, 0, 0.2, 0), (72, -0.5, 0, -0.4, 0, -0.5, 0),
                    (96, 0, 0, 0, 0, 0.0, 0)]
    idle["torso"] = [(0, 1.5, 0, 0, 0, 0, 0), (30, 0.2, -2.0, 0.8, 0, 0, 0),
                     (60, 2.2, 1.5, -0.6, 0, 0, 0), (96, 1.5, 0, 0, 0, 0, 0)]
    idle["head"] = [(0, 0, 6, 0, 0, 0, 0), (20, -2, 9, 1.5, 0, 0, 0),
                    (44, 1, -7, -1.0, 0, 0, 0), (66, -1, -9, 0.5, 0, 0, 0),
                    (96, 0, 6, 0, 0, 0, 0)]
    idle["armR"] = [(0, 0, 0, 0, 0, 0, 0), (48, 3.5, 0, 2.0, 0, 0, 0),
                    (96, 0, 0, 0, 0, 0, 0)]
    idle["armL"] = [(0, -4, 0, 0, 0, 0, 0), (40, -1, 0, -2.5, 0, 0, 0),
                    (96, -4, 0, 0, 0, 0, 0)]
    idle["gun"] = [(0, 0, 0, 0, 0, 0, 0), (52, -2.5, 1.5, 0, 0, 0, 0),
                   (96, 0, 0, 0, 0, 0, 0)]
    legs_pass(idle, [(0, 0, 0, 0), (24, 1.6, 1.1, -0.9), (48, -0.8, -1.4, 0.7),
                     (72, -1.8, 0.6, -0.5), (96, 0, 0, 0)],
              front=1.0, rear=0.7, overwrite=True)
    A["idle"] = {"frames": F, "loop": 1, "tracks": idle}

    # --- walk.  Lateral sequence, 48 frames a stride.  Diagonal pairs are half
    # a cycle apart, which is what stops a four-legged walk reading as a pair of
    # scissors.  The hull bobs at twice stride frequency because two feet land
    # per cycle, and rolls toward whichever side is bearing.
    walk = {}
    W = 48
    PHASE = {("L", "F"): 0.00, ("R", "B"): 0.25,
             ("R", "F"): 0.50, ("L", "B"): 0.75}
    for side in (1, -1):
        for pair in ("front", "rear"):
            g = leg_name(side, pair)
            ph = PHASE[(g[0], g[1])]
            _gait(walk, g, side, ph, W, swing=9.0, lift=6.0, knee=0.55, bias=3.0)
    walk["hull"] = [(0, 0, 0, 2.6, 0, 0, 0), (12, 1.4, 0, 0, 0, -2.2, 0),
                    (24, 0, 0, -2.6, 0, 0.4, 0), (36, -1.4, 0, 0, 0, -2.2, 0),
                    (48, 0, 0, 2.6, 0, 0, 0)]
    walk["torso"] = [(0, 2, 3.5, -1.8, 0, 0, 0), (12, 4, 0, 0, 0, 0, 0),
                     (24, 2, -3.5, 1.8, 0, 0, 0), (36, 4, 0, 0, 0, 0, 0),
                     (48, 2, 3.5, -1.8, 0, 0, 0)]
    walk["head"] = [(0, 0, -3, 1.6, 0, 0, 0), (24, 0, 3, -1.6, 0, 0, 0),
                    (48, 0, -3, 1.6, 0, 0, 0)]          # counters the torso
    walk["armR"] = [(0, -9, 0, 0, 0, 0, 0), (24, 7, 0, 3, 0, 0, 0),
                    (48, -9, 0, 0, 0, 0, 0)]
    walk["armL"] = [(0, 6, 0, -2, 0, 0, 0), (24, -8, 0, 0, 0, 0, 0),
                    (48, 6, 0, -2, 0, 0, 0)]
    walk["gun"] = [(0, 3, -2, 0, 0, 0, 0), (24, -4, 2, 0, 0, 0, 0),
                   (48, 3, -2, 0, 0, 0, 0)]
    A["walk"] = {"frames": W, "loop": 1, "tracks": walk}

    # --- fire.  Anticipation, discharge, recovery.  The recoil is three frames
    # of hard kick and eighteen of the chassis absorbing it, because the whole
    # point of bolting a shotgun to a man is that he cannot brace against it.
    fire = {}
    fire["gun"] = [(0, 0, 0, 0, 0, 0, 0), (6, 6, 0, 0, 0, 0, -1.0),
                   (10, -3, 0, 0, 0, 0, 0), (12, -26, 0, 0, 0, 2.0, 9.0),
                   (15, -14, 0, 0, 0, 1.0, 4.0), (22, 2, 0, 0, 0, 0, -0.5),
                   (34, 0, 0, 0, 0, 0, 0)]
    fire["armL"] = [(0, -4, 0, 0, 0, 0, 0), (6, -16, -4, -3, 0, 0, 0),
                    (10, -20, -5, -4, 0, 0, 0), (12, -6, -2, 2, 0, 0, 0),
                    (18, -13, -4, -1, 0, 0, 0), (34, -4, 0, 0, 0, 0, 0)]
    fire["torso"] = [(0, 1.5, 0, 0, 0, 0, 0), (8, -1, -6, 2, 0, 0, 0),
                     (12, 7, 4, -3, 0, 0, 0), (20, 2, -2, 1, 0, 0, 0),
                     (34, 1.5, 0, 0, 0, 0, 0)]
    fire["head"] = [(0, 0, -4, 0, 0, 0, 0), (6, 2, -9, -2, 0, 0, 0),
                    (12, -4, -6, 3, 0, 0, 0), (34, 0, -4, 0, 0, 0, 0)]
    fire["hull"] = [(0, 0, 0, 0, 0, 0, 0), (12, -3.5, 0, 0, 0, 0, 3.5),
                    (20, 1.2, 0, 0, 0, -0.8, -1.0), (34, 0, 0, 0, 0, 0, 0)]
    legs_pass(fire, [(0, 0, 0, 0), (8, -2.5, 1.5, -1.0),
                     (12, 7.0, -5.5, 4.0), (18, 3.0, -2.0, 1.5),
                     (24, -1.5, 1.0, -0.8), (34, 0, 0, 0)],
              front=1.0, rear=0.55, overwrite=True)
    A["fire"] = {"frames": 34, "loop": 0, "tracks": fire}

    # --- notice.  The head snaps and the chassis rises before anything else
    # moves.  It is the animation that has to sell that he saw you.
    notice = {}
    notice["head"] = [(0, 0, 14, 0, 0, 0, 0), (5, -6, -16, -5, 0, 0, 0),
                      (8, -9, -20, -6, 0, 0, 0), (14, -7, -18, -5, 0, 0, 0),
                      (30, -7, -18, -5, 0, 0, 0)]
    notice["torso"] = [(0, 1.5, 4, 0, 0, 0, 0), (9, -3, -8, 0, 0, 0, 0),
                       (16, -1, -6, 0, 0, 0, 0), (30, -1, -6, 0, 0, 0, 0)]
    notice["hull"] = [(0, 0, 0, 0, 0, 0, 0), (10, -2.5, 0, 0, 0, 3.0, -1.5),
                      (18, -1.0, 0, 0, 0, 2.0, -0.5), (30, -1.0, 0, 0, 0, 2.0, -0.5)]
    notice["armL"] = [(0, -4, 0, 0, 0, 0, 0), (10, -14, -6, -4, 0, 0, 0),
                      (30, -12, -5, -3, 0, 0, 0)]
    notice["armR"] = [(0, 0, 0, 0, 0, 0, 0), (10, -6, 0, 4, 0, 0, 0),
                      (30, -5, 0, 3, 0, 0, 0)]
    legs_pass(notice, [(0, 0, 0, 0), (6, 2.0, -1.5, 1.2),
                       (10, -7.0, 4.5, -3.0), (18, -5.0, 3.0, -2.0),
                       (30, -5.0, 3.0, -2.0)],
              front=1.0, rear=0.8, overwrite=True)
    A["notice"] = {"frames": 30, "loop": 0, "tracks": notice}

    # --- claw work.  Three helpers first, because a grab and a slash and both
    # combos all need the same hand shapes and they must agree exactly — a
    # combo whose claw poses drift from the standalone slash reads as two
    # different weapons.
    def claws(tracks, keys):
        """keys = [(frame, spread_deg, curl_deg), ...] applied to all three.

        The claws hinge at the knuckle: `spread` rotates them off the barrel
        axis to open the hand, `curl` closes them past it to clamp.  Claw 0 is
        the top finger and lags the other two slightly on the close, so the
        hand shuts like a hand and not like a vice.
        """
        for k in range(3):
            lag = 1.0 if k else 0.82
            tracks["claw%d" % k] = [
                (f, sp * lag, 0.0, -cu * lag, 0, 0, 0) for (f, sp, cu) in keys]

    OPEN = 34.0     # how far the hand splays before it takes hold
    SHUT = -16.0    # and how far past the barrel it closes

    # --- grab.  Reach, open, take, crush.  The hold at the end is deliberate:
    # this is the animation a grab-and-hold state machine loops on.
    grab = {}
    grab["armL"] = [(0, -4, 0, 0, 0, 0, 0), (5, -22, -10, -6, 0, 0, 0),
                    (12, -30, -16, -9, 0, 0, 0), (18, -26, -14, -7, 0, 0, 0),
                    (30, -26, -14, -7, 0, 0, 0)]
    grab["gun"] = [(0, 0, 0, 0, 0, 0, 0), (5, 8, 0, 0, 0, 0, -2.0),
                   (12, -4, 0, 0, 0, 0, 7.0), (18, 0, 0, 0, 0, 0, 5.0),
                   (30, 0, 0, 0, 0, 0, 5.0)]
    claws(grab, [(0, 0, 0), (6, OPEN, 0), (11, OPEN, 0),
                 (15, 0, SHUT), (20, 0, SHUT * 1.3), (30, 0, SHUT * 1.3)])
    grab["torso"] = [(0, 1.5, 0, 0, 0, 0, 0), (10, 3, -9, -3, 0, 0, 0),
                     (16, 6, -4, 1, 0, 0, 0), (30, 5, -6, 0, 0, 0, 0)]
    grab["head"] = [(0, 0, -4, 0, 0, 0, 0), (10, -5, -12, -3, 0, 0, 0),
                    (30, -4, -10, -2, 0, 0, 0)]
    grab["hull"] = [(0, 0, 0, 0, 0, 0, 0), (12, -2, 0, 0, 0, 0, 2.5),
                    (30, -1, 0, 0, 0, 0, 1.5)]
    legs_pass(grab, [(0, 0, 0, 0), (6, -3.0, 1.5, -1.0), (12, -7.0, 4.0, -2.5),
                     (18, -5.0, 3.0, -2.0), (30, -5.0, 3.0, -2.0)],
              front=1.0, rear=0.6)
    A["grab"] = {"frames": 30, "loop": 0, "tracks": grab}

    # --- slash.  Wind up across the body, then throw the whole chassis into
    # it.  The claws stay open through the swing — a closed hand is a punch —
    # and the follow-through overshoots before it settles, because nothing
    # this heavy stops where it means to.
    slash = {}
    slash["armL"] = [(0, -4, 0, 0, 0, 0, 0), (7, 10, 26, 18, 0, 0, 0),
                     (10, 6, 20, 14, 0, 0, 0), (16, -24, -34, -22, 0, 0, 0),
                     (20, -18, -42, -26, 0, 0, 0), (28, -4, -6, -2, 0, 0, 0)]
    slash["gun"] = [(0, 0, 0, 0, 0, 0, 0), (7, 0, 14, 0, 0, 0, 0),
                    (16, 0, -20, 0, 0, 0, 0), (20, 0, -26, 0, 0, 0, 0),
                    (28, 0, 0, 0, 0, 0, 0)]
    claws(slash, [(0, 0, 0), (6, OPEN * 0.9, 0), (20, OPEN, 0),
                  (28, 0, 0)])
    slash["torso"] = [(0, 1.5, 0, 0, 0, 0, 0), (7, 0, 16, 6, 0, 0, 0),
                      (16, 4, -18, -7, 0, 0, 0), (20, 5, -22, -8, 0, 0, 0),
                      (28, 1.5, 0, 0, 0, 0, 0)]
    slash["head"] = [(0, 0, -4, 0, 0, 0, 0), (7, 0, 10, 0, 0, 0, 0),
                     (16, -3, -14, -4, 0, 0, 0), (28, 0, -4, 0, 0, 0, 0)]
    slash["hull"] = [(0, 0, 0, 0, 0, 0, 0), (7, 0, 6, 1.5, 0, 0, -1.5),
                     (16, 0, -8, -2.0, 0, 0, 2.5), (28, 0, 0, 0, 0, 0, 0)]
    # the turn goes all the way down: front feet step under it, rear drive it
    legs_pass(slash, [(0, 0, 0, 0), (7, -6.0, 3.0, -2.0),
                      (16, 8.0, -4.5, 3.0), (20, 6.0, -3.0, 2.0),
                      (28, 0, 0, 0)],
              front=1.0, rear=0.75, mirror=False, overwrite=True)
    A["slash"] = {"frames": 28, "loop": 0, "tracks": slash}

    # --- combo A: slash, slash, shoot.  Two swings to close the distance and
    # a barrel in your chest to finish.  The recoil lands on frame 46 with the
    # arm already extended, so the kick has nowhere to go but through him.
    ca = {}
    ca["armL"] = [(0, -4, 0, 0, 0, 0, 0), (6, 8, 24, 16, 0, 0, 0),
                  (14, -22, -32, -20, 0, 0, 0),          # first swing, outward
                  (20, 4, -20, 10, 0, 0, 0),
                  (28, -20, 30, -18, 0, 0, 0),           # backhand, returning
                  (36, -14, 6, -6, 0, 0, 0),
                  (42, -20, -6, -4, 0, 0, 0),            # level the barrel
                  (46, -6, -2, 2, 0, 0, 0),              # kick
                  (52, -15, -5, -2, 0, 0, 0), (64, -4, 0, 0, 0, 0, 0)]
    ca["gun"] = [(0, 0, 0, 0, 0, 0, 0), (6, 0, 12, 0, 0, 0, 0),
                 (14, 0, -18, 0, 0, 0, 0), (28, 0, 22, 0, 0, 0, 0),
                 (42, 4, 0, 0, 0, 0, -1.0),
                 (46, -26, 0, 0, 0, 2.0, 9.0),           # discharge
                 (50, -12, 0, 0, 0, 1.0, 3.5),
                 (56, 2, 0, 0, 0, 0, -0.5), (64, 0, 0, 0, 0, 0, 0)]
    claws(ca, [(0, 0, 0), (5, OPEN, 0), (32, OPEN, 0),
               (40, OPEN * 0.35, 0), (46, 0, SHUT * 0.5), (64, 0, 0)])
    ca["torso"] = [(0, 1.5, 0, 0, 0, 0, 0), (6, 0, 15, 6, 0, 0, 0),
                   (14, 4, -17, -7, 0, 0, 0), (28, 2, 14, 5, 0, 0, 0),
                   (42, -1, -7, 2, 0, 0, 0), (46, 8, 5, -3, 0, 0, 0),
                   (54, 2, -2, 1, 0, 0, 0), (64, 1.5, 0, 0, 0, 0, 0)]
    ca["head"] = [(0, 0, -4, 0, 0, 0, 0), (6, 0, 9, 0, 0, 0, 0),
                  (14, -3, -13, -4, 0, 0, 0), (28, 0, 8, 0, 0, 0, 0),
                  (42, 1, -10, -2, 0, 0, 0), (46, -5, -7, 3, 0, 0, 0),
                  (64, 0, -4, 0, 0, 0, 0)]
    ca["hull"] = [(0, 0, 0, 0, 0, 0, 0), (6, 0, 5, 1.2, 0, 0, -1.5),
                  (14, 0, -7, -1.8, 0, 0, 2.0), (28, 0, 6, 1.5, 0, 0, -1.0),
                  (46, -3.5, 0, 0, 0, 0, 3.5), (54, 1.2, 0, 0, 0, -0.8, -1.0),
                  (64, 0, 0, 0, 0, 0, 0)]
    legs_pass(ca, [(0, 0, 0, 0), (6, -5.0, 2.5, -1.5), (14, 7.0, -4.0, 2.5),
                   (20, 2.0, -1.0, 0.5), (28, -6.0, 3.0, -2.0),
                   (36, 1.0, -0.5, 0.5), (42, -3.0, 1.5, -1.0),
                   (46, 8.0, -5.5, 4.0), (54, 2.0, -1.5, 1.0), (64, 0, 0, 0)],
              front=1.0, rear=0.6, mirror=False)
    A["combo_slash_shoot"] = {"frames": 64, "loop": 0, "tracks": ca}

    # --- combo B: shoot, then slash through the smoke.  The opposite order and
    # a different animal — the shot buys the range, the claw closes it.  The
    # swing starts before the recoil has finished settling, on purpose.
    cb = {}
    cb["armL"] = [(0, -4, 0, 0, 0, 0, 0), (6, -18, -6, -4, 0, 0, 0),
                  (10, -22, -8, -5, 0, 0, 0),
                  (12, -6, -3, 2, 0, 0, 0),              # kick
                  (18, -16, -4, -2, 0, 0, 0),
                  (24, 10, 28, 18, 0, 0, 0),             # wind up out of it
                  (34, -24, -36, -24, 0, 0, 0),          # and through
                  (38, -18, -44, -28, 0, 0, 0), (50, -4, 0, 0, 0, 0, 0)]
    cb["gun"] = [(0, 0, 0, 0, 0, 0, 0), (6, 6, 0, 0, 0, 0, -1.0),
                 (10, -3, 0, 0, 0, 0, 0),
                 (12, -28, 0, 0, 0, 2.2, 9.5),           # discharge
                 (16, -13, 0, 0, 0, 1.0, 4.0),
                 (24, 0, 16, 0, 0, 0, 0), (34, 0, -22, 0, 0, 0, 0),
                 (38, 0, -28, 0, 0, 0, 0), (50, 0, 0, 0, 0, 0, 0)]
    claws(cb, [(0, 0, 0), (10, 0, SHUT * 0.4), (18, OPEN * 0.5, 0),
               (24, OPEN, 0), (38, OPEN, 0), (50, 0, 0)])
    cb["torso"] = [(0, 1.5, 0, 0, 0, 0, 0), (8, -1, -6, 2, 0, 0, 0),
                   (12, 7, 4, -3, 0, 0, 0), (20, 2, -2, 1, 0, 0, 0),
                   (24, 0, 17, 6, 0, 0, 0), (34, 4, -19, -8, 0, 0, 0),
                   (38, 5, -23, -9, 0, 0, 0), (50, 1.5, 0, 0, 0, 0, 0)]
    cb["head"] = [(0, 0, -4, 0, 0, 0, 0), (6, 2, -9, -2, 0, 0, 0),
                  (12, -4, -6, 3, 0, 0, 0), (24, 0, 11, 0, 0, 0, 0),
                  (34, -3, -15, -4, 0, 0, 0), (50, 0, -4, 0, 0, 0, 0)]
    cb["hull"] = [(0, 0, 0, 0, 0, 0, 0), (12, -3.5, 0, 0, 0, 0, 3.5),
                  (20, 1.2, 0, 0, 0, -0.8, -1.0), (24, 0, 6, 1.5, 0, 0, -1.5),
                  (34, 0, -8, -2.0, 0, 0, 2.5), (50, 0, 0, 0, 0, 0, 0)]
    legs_pass(cb, [(0, 0, 0, 0), (8, -2.5, 1.5, -1.0), (12, 8.0, -5.5, 4.0),
                   (20, 2.0, -1.5, 1.0), (24, -6.0, 3.0, -2.0),
                   (34, 8.0, -4.5, 3.0), (38, 6.0, -3.0, 2.0), (50, 0, 0, 0)],
              front=1.0, rear=0.6, mirror=False)
    A["combo_shoot_slash"] = {"frames": 50, "loop": 0, "tracks": cb}

    # --- overhand.  The one move where the chassis matters more than the arm.
    #
    # Order of operations is the whole thing: the hull loads onto the rear legs
    # and pitches nose-up on the wind-up, so the mass is *behind* the front
    # feet before the arm ever leaves the top.  Then the strike throws it all
    # forward at once — arm down, torso pitched over, hull surging onto the
    # front legs, which compress to take it.  Do it in the other order and the
    # CoM crosses the front feet on frame 20 and he falls on his face; that is
    # what balance_check() is watching for.
    oh = {}
    oh["armL"] = [(0, -4, 0, 0, 0, 0, 0),
                  (9, 26, 8, 58, 0, 0, 0),               # up and over the head
                  (14, 32, 10, 66, 0, 0, 0),             # top of the arc, held
                  (17, 30, 8, 62, 0, 0, 0),              # settle — the tell
                  (23, -46, -6, 16, 0, 0, 0),            # down through it
                  (27, -58, -10, 4, 0, 0, 0),            # follow-through
                  (34, -40, -6, 6, 0, 0, 0), (46, -4, 0, 0, 0, 0, 0)]
    oh["gun"] = [(0, 0, 0, 0, 0, 0, 0), (14, 18, 0, 0, 0, 0, 0),
                 (23, -20, 0, 0, 0, 0, 0), (27, -30, 0, 0, 0, 0, 0),
                 (46, 0, 0, 0, 0, 0, 0)]
    claws(oh, [(0, 0, 0), (8, OPEN, 0), (27, OPEN, 0), (34, OPEN * 0.6, 0),
               (46, 0, 0)])
    oh["torso"] = [(0, 1.5, 0, 0, 0, 0, 0),
                   (12, -17, 4, 3, 0, 0, 0),             # arched back
                   (17, -15, 5, 3, 0, 0, 0),
                   (24, 30, -3, -4, 0, 0, 0),            # thrown over the front
                   (28, 35, -4, -5, 0, 0, 0),
                   (36, 8, 0, 0, 0, 0, 0), (46, 1.5, 0, 0, 0, 0, 0)]
    oh["head"] = [(0, 0, -4, 0, 0, 0, 0), (12, -8, -6, -2, 0, 0, 0),
                  (24, 12, -2, 2, 0, 0, 0), (28, 14, -3, 3, 0, 0, 0),
                  (46, 0, -4, 0, 0, 0, 0)]
    oh["armR"] = [(0, 0, 0, 0, 0, 0, 0), (12, 14, 0, -8, 0, 0, 0),
                  (24, -16, 0, 6, 0, 0, 0), (46, 0, 0, 0, 0, 0, 0)]
    # the chassis: back and up to load, then forward and down to deliver
    # Hull pitch stays small.  The legs hang off the hull, so every degree of
    # chassis rotation levers the far feet off the deck — 6.5 deg lifted the
    # rear pair 15 cm into the air and left him balancing on the front two,
    # which is how the CoM ended up outside the support at frame 29.  The
    # weight read lives in the torso instead, which is a sibling of the legs
    # and moves nothing underneath him.
    oh["hull"] = [(0, 0, 0, 0, 0, 0, 0),
                  (12, -1.8, 0, 0, 0, 0.5, -7.0),        # weight onto the rear
                  (17, -2.0, 0, 0, 0, 0.6, -8.0),
                  (25, 2.4, 0, 0, 0, -0.7, 7.5),         # and onto the front
                  (30, 1.5, 0, 0, 0, -0.4, 5.0),
                  (46, 0, 0, 0, 0, 0, 0)]
    for side in (1, -1):
        gf, gb = leg_name(side, "front"), leg_name(side, "rear")
        # front legs unload on the wind-up, then compress under the landing
        oh[gf + "_hip"] = [(0, 0, 0, 0, 0, 0, 0), (12, 0, 4, 2 * side, 0, 0, 0),
                           (25, 0, -6, -5 * side, 0, 0, 0),
                           (32, 0, -2, -2 * side, 0, 0, 0),
                           (46, 0, 0, 0, 0, 0, 0)]
        # rear legs take it first, then extend to drive him through the swing
        oh[gb + "_hip"] = [(0, 0, 0, 0, 0, 0, 0), (12, 0, -3, -4 * side, 0, 0, 0),
                           (17, 0, -4, -5 * side, 0, 0, 0),
                           (25, 0, 5, 3 * side, 0, 0, 0),
                           (46, 0, 0, 0, 0, 0, 0)]
        oh[gb + "_shin"] = [(0, 0, 0, 0, 0, 0, 0), (17, 0, 0, -3 * side, 0, 0, 0),
                            (25, 0, 0, 4 * side, 0, 0, 0),
                            (46, 0, 0, 0, 0, 0, 0)]
    A["slash_overhand"] = {"frames": 46, "loop": 0, "tracks": oh}

    # --- straight right.  The mitt is the only part of him still shaped like a
    # hand, and it weighs what a bench vice weighs.  A punch is a rotation, not
    # an arm extension: the torso yaw is what carries it, and the arm mostly
    # goes along.  His right shoulder is at -X, so positive torso yaw is what
    # drives that side forward.
    pr = {}
    pr["torso"] = [(0, 1.5, 0, 0, 0, 0, 0),
                   (8, 0, -14, -3, 0, 0, 0),             # chamber, coiled off
                   (11, 0, -16, -3, 0, 0, 0),
                   (16, 3, 20, 4, 0, 0, 0),              # uncoil
                   (19, 4, 24, 5, 0, 0, 0),              # peak, arm is straight
                   (26, 2, 8, 2, 0, 0, 0), (38, 1.5, 0, 0, 0, 0, 0)]
    pr["armR"] = [(0, 0, 0, 0, 0, 0, 0),
                  (8, 22, -6, -10, 0, 0, 0),             # elbow drawn back
                  (11, 26, -8, -12, 0, 0, 0),
                  (16, -34, 6, 4, 0, 0, 0),              # thrown
                  (19, -44, 8, 6, 0, 0, 0),              # full extension
                  (24, -30, 4, 2, 0, 0, 0),              # and recoiling off it
                  (38, 0, 0, 0, 0, 0, 0)]
    pr["armL"] = [(0, -4, 0, 0, 0, 0, 0), (8, -10, 10, -4, 0, 0, 0),
                  (19, 6, -16, 4, 0, 0, 0),              # counter-rotates
                  (38, -4, 0, 0, 0, 0, 0)]
    pr["head"] = [(0, 0, -4, 0, 0, 0, 0), (8, -2, 6, 0, 0, 0, 0),
                  (16, 4, -10, -2, 0, 0, 0), (19, 5, -12, -2, 0, 0, 0),
                  (38, 0, -4, 0, 0, 0, 0)]
    pr["hull"] = [(0, 0, 0, 0, 0, 0, 0),
                  (10, -1.2, -5, 0, 0, 0.4, -5.0),       # sit back to load
                  (19, 1.9, 7, 0, 0, -0.6, 8.5),         # and drive through
                  (26, 0.8, 2, 0, 0, -0.3, 3.0),
                  (38, 0, 0, 0, 0, 0, 0)]
    for side in (1, -1):
        gf, gb = leg_name(side, "front"), leg_name(side, "rear")
        pr[gf + "_hip"] = [(0, 0, 0, 0, 0, 0, 0), (10, 0, 3, 1 * side, 0, 0, 0),
                           (19, 0, -7, -4 * side, 0, 0, 0),
                           (38, 0, 0, 0, 0, 0, 0)]
        # the drive comes off his right rear leg — that is the one that pushes
        drive = 1.6 if side < 0 else 0.7
        pr[gb + "_hip"] = [(0, 0, 0, 0, 0, 0, 0),
                           (10, 0, -4 * drive, -3 * side, 0, 0, 0),
                           (19, 0, 7 * drive, 4 * side, 0, 0, 0),
                           (38, 0, 0, 0, 0, 0, 0)]
    A["punch_right"] = {"frames": 38, "loop": 0, "tracks": pr}

    # --- jump.  Crouch, launch, tuck, land.  Four legs means he does not
    # spring so much as fold and unfold, and the landing is the interesting
    # half: all four hit at once and the chassis has to eat the whole drop,
    # so the recovery runs twice as long as the launch.
    jp = {}
    # Hull translation and leg roll both move the feet, and the first pass
    # applied the full amount of each: the crouch put all four 27 cm under the
    # deck.  The hull carries the body's rise and fall; the legs carry only the
    # difference between that and where the feet must stay.
    legs_pass(jp, [(0, 0, 0, 0),
                   (10, 2.0, -3.6, 2.4),      # compress — knees out, body down
                   (14, 1.4, -4.4, 3.0),
                   (19, -2.5, 4.5, -3.0),     # extend hard
                   (23, -3.5, 5.5, -3.6),
                   (28, 2.0, -6.0, 9.0),      # tuck in the air
                   (34, 0.0, 5.0, -3.2),      # reach for the deck
                   (38, 2.0, -4.2, 3.2),      # absorb
                   (44, 0.6, -1.8, 1.4),
                   (56, 0, 0, 0)],
              front=1.0, rear=0.92, overwrite=True)
    jp["hull"] = [(0, 0, 0, 0, 0, 0, 0), (10, 2.0, 0, 0, 0, -4.5, 0),
                  (14, 2.5, 0, 0, 0, -5.5, 0),
                  (23, -4.0, 0, 0, 0, 26.0, 6.0),      # airborne, nose up
                  (28, -2.0, 0, 0, 0, 33.0, 10.0),     # apex
                  (34, 3.0, 0, 0, 0, 14.0, 6.0),
                  (38, 5.0, 0, 0, 0, -4.5, 1.0),       # impact
                  (44, 1.0, 0, 0, 0, -2.0, 0), (56, 0, 0, 0, 0, 0, 0)]
    jp["torso"] = [(0, 1.5, 0, 0, 0, 0, 0), (12, 12, 0, 0, 0, 0, 0),
                   (23, -14, 0, 0, 0, 0, 0), (28, -8, 0, 0, 0, 0, 0),
                   (38, 16, 0, 0, 0, 0, 0), (46, 4, 0, 0, 0, 0, 0),
                   (56, 1.5, 0, 0, 0, 0, 0)]
    jp["head"] = [(0, 0, -4, 0, 0, 0, 0), (12, 8, -4, 0, 0, 0, 0),
                  (23, -12, -3, 0, 0, 0, 0), (38, 10, -4, 0, 0, 0, 0),
                  (56, 0, -4, 0, 0, 0, 0)]
    jp["armR"] = [(0, 0, 0, 0, 0, 0, 0), (12, 20, 0, -10, 0, 0, 0),
                  (23, -26, 0, 12, 0, 0, 0), (38, 14, 0, -6, 0, 0, 0),
                  (56, 0, 0, 0, 0, 0, 0)]
    jp["armL"] = [(0, -4, 0, 0, 0, 0, 0), (12, 16, 0, 14, 0, 0, 0),
                  (23, -22, 0, -8, 0, 0, 0), (38, 10, 0, 6, 0, 0, 0),
                  (56, -4, 0, 0, 0, 0, 0)]
    jp["jaw"] = [(0, 0, 0, 0, 0, 0, 0), (19, 14, 0, 0, 0, 0, 0),
                 (30, 6, 0, 0, 0, 0, 0), (38, 18, 0, 0, 0, 0, 0),
                 (48, 0, 0, 0, 0, 0, 0)]
    A["jump"] = {"frames": 56, "loop": 0, "tracks": jp}

    # --- talk.  A loop, because dialogue runs as long as the text box does.
    #
    # The jaw does not open and close on a metronome — speech is a rhythm of
    # long open vowels and clipped consonant closes, so the keys are spaced
    # unevenly and never quite return to shut.  The head drifts against it:
    # nobody speaks with their skull locked, and on a face with one working
    # eye the head *is* the eyebrow.
    tk = {}
    tk["jaw"] = [(0, 2, 0, 0, 0, 0, 0), (4, 15, 0, 0, 0, 0, 0),
                 (8, 5, 0, 0, 0, 0, 0), (11, 19, 0, 0, 0, 0, 0),
                 (15, 3, 0, 0, 0, 0, 0), (18, 9, 0, 0, 0, 0, 0),
                 (21, 2, 0, 0, 0, 0, 0), (26, 17, 0, 0, 0, 0, 0),
                 (30, 6, 0, 0, 0, 0, 0), (34, 12, 0, 0, 0, 0, 0),
                 (38, 2, 0, 0, 0, 0, 0), (42, 14, 0, 0, 0, 0, 0),
                 (46, 4, 0, 0, 0, 0, 0), (48, 2, 0, 0, 0, 0, 0)]
    tk["head"] = [(0, 0, -4, 0, 0, 0, 0), (12, -3, -1, -1.5, 0, 0, 0),
                  (24, 2, -7, 1.0, 0, 0, 0), (36, -2, -2, -0.8, 0, 0, 0),
                  (48, 0, -4, 0, 0, 0, 0)]
    tk["torso"] = [(0, 1.5, 0, 0, 0, 0, 0), (16, 2.5, -2, 0.6, 0, 0, 0),
                   (32, 0.8, 2, -0.5, 0, 0, 0), (48, 1.5, 0, 0, 0, 0, 0)]
    tk["armL"] = [(0, -4, 0, 0, 0, 0, 0), (20, -7, 3, -2, 0, 0, 0),
                  (48, -4, 0, 0, 0, 0, 0)]
    tk["armR"] = [(0, 0, 0, 0, 0, 0, 0), (14, -4, 0, 3, 0, 0, 0),
                  (34, 2, 0, -1, 0, 0, 0), (48, 0, 0, 0, 0, 0, 0)]
    legs_pass(tk, [(0, 0, 0, 0), (16, 1.2, 0.8, -0.6), (32, -1.0, -1.0, 0.5),
                   (48, 0, 0, 0)], front=1.0, rear=0.6)
    A["talk"] = {"frames": 48, "loop": 1, "tracks": tk}

    # --- shout.  The same rig pushed until it stops being speech: jaw wide and
    # held, chassis braced against it, everything leaning in.  This is the one
    # to trigger on a boss intro.
    sh = {}
    sh["jaw"] = [(0, 0, 0, 0, 0, 0, 0), (5, 26, 0, 0, 0, 0, 0),
                 (9, 31, 0, 0, 0, 0, 0), (24, 28, 0, 0, 0, 0, 0),
                 (30, 30, 0, 0, 0, 0, 0), (38, 4, 0, 0, 0, 0, 0),
                 (44, 0, 0, 0, 0, 0, 0)]
    sh["head"] = [(0, 0, -4, 0, 0, 0, 0), (6, -14, -3, -2, 0, 0, 0),
                  (14, -17, -2, -3, 0, 0, 0), (28, -15, -4, -2, 0, 0, 0),
                  (44, 0, -4, 0, 0, 0, 0)]
    sh["torso"] = [(0, 1.5, 0, 0, 0, 0, 0), (8, -9, 0, 0, 0, 0, 0),
                   (16, -11, 2, 1, 0, 0, 0), (30, -9, -2, -1, 0, 0, 0),
                   (44, 1.5, 0, 0, 0, 0, 0)]
    sh["armL"] = [(0, -4, 0, 0, 0, 0, 0), (8, 6, 14, 22, 0, 0, 0),
                  (20, 4, 10, 26, 0, 0, 0), (44, -4, 0, 0, 0, 0, 0)]
    sh["armR"] = [(0, 0, 0, 0, 0, 0, 0), (8, 10, -6, -18, 0, 0, 0),
                  (20, 8, -4, -22, 0, 0, 0), (44, 0, 0, 0, 0, 0, 0)]
    claws(sh, [(0, 0, 0), (8, OPEN, 0), (30, OPEN, 0), (44, 0, 0)])
    legs_pass(sh, [(0, 0, 0, 0), (8, -4.0, 3.5, -2.5), (18, -6.0, 5.0, -3.5),
                   (30, -5.0, 4.0, -3.0), (44, 0, 0, 0)],
              front=1.0, rear=0.85)
    A["shout"] = {"frames": 44, "loop": 0, "tracks": sh}
    return A


def _gait(bone_keys, name, side, phase, frames, swing, lift, knee,
          bias=0.0):
    """One leg's contribution to a walk cycle, offset by `phase` of a stride.

    Axis choice is the whole trick here and it is not the obvious one.  These
    legs strike out sideways along X and only then drop, so the pivot-to-foot
    vector is mostly lateral: fore/aft swing is *yaw* about Y, and lifting the
    foot is *roll* about Z, which is the reverse of what a normal biped limb
    wants.  Rolling for stride and yawing for lift — the first thing I wrote —
    gives you a leg that scissors sideways and rows the ground.

    Roll also has to be signed by which side the leg is on: +Z rotation carries
    +X upward and -X downward, so his right legs need the opposite sign to lift
    at all.

    Lateral sequence: each foot lifts a quarter-stride after the one diagonally
    behind it.  Stance is the first half of the cycle, swing the second.
    """
    hip, boom, shin = [], [], []
    for k in range(9):
        t = k / 8.0
        f = int(round(t * frames))
        th = 2.0 * math.pi * ((t + phase) % 1.0)
        fore = swing * math.cos(th)                       # + is forward
        up = lift * max(0.0, -math.sin(th)) ** 1.2        # swing half only
        # `bias` holds the stance foot on the deck.  Without it the rest pose
        # is the lowest point of the cycle and every planted foot sinks
        # through the floor by however much the swing lifts.
        # Yaw is signed by side too, and for the same reason as roll: a +Y
        # rotation carries +X backward but -X forward, so feeding both sides
        # the same number walks his left legs one way and his right legs the
        # other, which looks like the two halves are arguing.
        hip.append((f, 0.0, -fore * side, (up + bias) * side, 0, 0, 0))
        boom.append((f, 0.0, -fore * 0.22 * side, -up * 0.30 * side, 0, 0, 0))
        shin.append((f, 0.0, 0.0, up * knee * side, 0, 0, 0))
    bone_keys[name + "_hip"] = hip
    bone_keys[name + "_boom"] = boom
    bone_keys[name + "_shin"] = shin


def sample_track(keys, f):
    """Linear interpolation between keyframes, clamped at both ends."""
    if f <= keys[0][0]:
        return keys[0][1:]
    if f >= keys[-1][0]:
        return keys[-1][1:]
    for i in range(len(keys) - 1):
        a, b = keys[i], keys[i + 1]
        if a[0] <= f <= b[0]:
            t = (f - a[0]) / float(b[0] - a[0]) if b[0] != a[0] else 0.0
            return tuple(a[1 + k] + (b[1 + k] - a[1 + k]) * t for k in range(6))
    return keys[-1][1:]


def pose_matrices(anim, f):
    """Evaluate every bone at frame f, returning name -> 4x4 world matrix.

    The same walk down BONE_ORDER the console does: local rotate about the
    bone's own pivot, then concatenate onto the parent.  Parents are guaranteed
    to come first, so one pass is enough.
    """
    import numpy as np

    out = {}
    for name in BONE_ORDER:
        b = BONES[name]
        keys = anim["tracks"].get(name)
        rx, ry, rz, tx, ty, tz = (sample_track(keys, f) if keys
                                  else (0.0,) * 6)
        cx, cy, cz = (math.cos(math.radians(v)) for v in (rx, ry, rz))
        sx, sy, sz = (math.sin(math.radians(v)) for v in (rx, ry, rz))
        Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
        Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
        Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
        R = Rz @ Ry @ Rx
        piv = np.array(b["pivot"])
        L = np.eye(4)
        L[:3, :3] = R
        L[:3, 3] = piv + np.array([tx, ty, tz]) - R @ piv
        out[name] = L if b["parent"] is None else out[b["parent"]] @ L
    return out


# ---------------------------------------------------------------------------
# emitters
# ---------------------------------------------------------------------------
def write_obj(outdir):
    path = os.path.join(outdir, "machine_centaur.obj")
    with open(path, "w") as f:
        f.write("# machine_centaur.obj — generated by machine_centaur_gen.py\n")
        f.write("# %d verts, %d tris\n" % (len(M.v), M.count()))
        f.write("mtllib machine_centaur.mtl\n")
        for (p, c) in zip(M.v, M.c):
            f.write("v %.3f %.3f %.3f %.4f %.4f %.4f\n" %
                    (p[0], p[1], p[2], c[0] / 255.0, c[1] / 255.0, c[2] / 255.0))
        for uv in M.uv:
            f.write("vt %.5f %.5f\n" % (uv[0], 1.0 - uv[1]))
        for (bn, mat), tris in M.groups.items():
            f.write("g %s__%s\nusemtl mat_%s\n" % (bn, mat, mat))
            for (a, b, c) in tris:
                f.write("f %d/%d %d/%d %d/%d\n" %
                        (a + 1, a + 1, b + 1, b + 1, c + 1, c + 1))
    return path


def write_mtl(outdir):
    path = os.path.join(outdir, "machine_centaur.mtl")
    tex = {"face": "mc_face.png", "hull": "mc_plate.png",
           "gore": "mc_gore.png"}
    with open(path, "w") as f:
        for g in sorted({mat for (_, mat) in M.groups}):
            f.write("newmtl mat_%s\nKd 1.0 1.0 1.0\nKa 0 0 0\nillum 1\n" % g)
            if g in tex:
                f.write("map_Kd %s\n" % tex[g])
            f.write("\n")
    return path


def chunk_group(tris, cache=32):
    """Greedy split into batches whose vertex set fits the F3DEX2 cache."""
    out, cur, verts = [], [], []
    vmap = {}
    for t in tris:
        need = [v for v in t if v not in vmap]
        if len(vmap) + len(set(need)) > cache:
            out.append((verts, cur))
            cur, verts, vmap = [], [], {}
            need = list(set(t))
        for v in dict.fromkeys(need):
            vmap[v] = len(verts)
            verts.append(v)
        cur.append(tuple(vmap[v] for v in t))
    if cur:
        out.append((verts, cur))
    return out


def write_header(outdir, scale, face_q, plate_q, gore_q):
    path = os.path.join(outdir, "machine_centaur.h")
    lines = []
    lines.append("/* machine_centaur.h — generated, do not hand-edit.")
    lines.append(" * %d verts, %d tris, scale = %d units/cm" %
                 (len(M.v), M.count(), scale))
    lines.append(" * %d bones, %d limb display lists, %d animations." %
                 (len(BONE_ORDER), len(M.groups), len(ANIMS)))
    lines.append(" *")
    lines.append(" * Rigid limb binding.  Walk mc_bones[] in order — parents")
    lines.append(" * always precede children — pushing each limb's matrix and")
    lines.append(" * running its display list, exactly like an F3DEX2 hierarchy.")
    lines.append(" * Rotations are s16 binary angles (0x4000 == 90 degrees);")
    lines.append(" * translations and pivots are in the same %d units/cm fixed" % scale)
    lines.append(" * point as the vertex data.")
    lines.append(" */")
    lines.append("#ifndef MACHINE_CENTAUR_H")
    lines.append("#define MACHINE_CENTAUR_H")
    lines.append("")
    lines.append("/* All three CI4 textures are painted on one 16-entry")
    lines.append(" * palette, so mc_face_tlut, mc_plate_tlut and mc_gore_tlut")
    lines.append(" * are byte-identical.  TLUTs live in the top half of TMEM:")
    lines.append(" * load one of them once at scene setup and none of the")
    lines.append(" * material switches below need to touch it again. */")
    lines.append(ci4_blob(face_q, "mc_face"))
    lines.append(ci4_blob(plate_q, "mc_plate"))
    lines.append(ci4_blob(gore_q, "mc_gore"))
    lines.append("")

    for (bn, g), tris in M.groups.items():
        batches = chunk_group(tris)
        key = "%s_%s" % (bn, g)
        vtx_name = "mc_vtx_%s" % key
        flat = []
        for verts, _ in batches:
            flat.extend(verts)
        # S/T are S10.5 texel coordinates, so the scale is the texture's own
        # size.  The face runs 64x64 and the hull 32x32; using one constant for
        # both is how you get a face that tiles twice across itself.
        tdim = {"face": FACE_TEX}.get(g, 32)
        lines.append("static const Vtx %s[] = {" % vtx_name)
        for v in flat:
            x, y, z = M.v[v]
            r, gg, b = M.c[v]
            s, t = M.uv[v]
            lines.append("    {{{ %5d, %5d, %5d }, 0, { %5d, %5d }, "
                         "{ %3d, %3d, %3d, 255 }}}," %
                         (int(round(x * scale)), int(round(y * scale)),
                          int(round(z * scale)),
                          int(round(s * (tdim - 1) * 32)),
                          int(round(t * (tdim - 1) * 32)),
                          r, gg, b))
        lines.append("};")
        lines.append("")

        lines.append("const Gfx mc_dl_%s[] = {" % key)
        base = 0
        for verts, batch in batches:
            lines.append("    gsSPVertex(%s + %d, %d, 0)," %
                         (vtx_name, base, len(verts)))
            i = 0
            while i + 1 < len(batch):
                a, b = batch[i], batch[i + 1]
                lines.append("    gsSP2Triangles(%d,%d,%d,0, %d,%d,%d,0)," %
                             (a[0], a[1], a[2], b[0], b[1], b[2]))
                i += 2
            if i < len(batch):
                a = batch[i]
                lines.append("    gsSP1Triangle(%d,%d,%d,0)," % a)
            base += len(verts)
        lines.append("    gsSPEndDisplayList(),")
        lines.append("};")
        lines.append("")

    # ---- the rig ----------------------------------------------------------
    lines.append("/* ---- rig ------------------------------------------------------ */")
    lines.append("typedef struct {")
    lines.append("    const char   *name;")
    lines.append("    signed char   parent;      /* index into mc_bones, -1 = root */")
    lines.append("    short         px, py, pz;  /* pivot, model space */")
    lines.append("    unsigned char ndl;")
    lines.append("    const Gfx    *dl[3];       /* this limb's display lists */")
    lines.append("} MCBone;")
    lines.append("")
    idx = {n: i for i, n in enumerate(BONE_ORDER)}
    per_bone = {}
    for (bn, g) in M.groups:
        per_bone.setdefault(bn, []).append("mc_dl_%s_%s" % (bn, g))
    lines.append("const MCBone mc_bones[%d] = {" % len(BONE_ORDER))
    for n in BONE_ORDER:
        b = BONES[n]
        dls = sorted(per_bone.get(n, []))
        lines.append('    { "%s", %2d, %6d, %6d, %6d, %d, { %s } },' %
                     (n, idx[b["parent"]] if b["parent"] else -1,
                      int(round(b["pivot"][0] * scale)),
                      int(round(b["pivot"][1] * scale)),
                      int(round(b["pivot"][2] * scale)),
                      len(dls),
                      ", ".join(dls + ["0"] * (3 - len(dls)))))
    lines.append("};")
    lines.append("")

    lines.append("/* ---- animation ----------------------------------------------- */")
    lines.append("typedef struct {")
    lines.append("    short frame;")
    lines.append("    short rx, ry, rz;   /* s16 binary angle, about the pivot */")
    lines.append("    short tx, ty, tz;")
    lines.append("} MCKey;")
    lines.append("typedef struct {")
    lines.append("    unsigned char bone, nkeys;")
    lines.append("    const MCKey  *keys;")
    lines.append("} MCTrack;")
    lines.append("typedef struct {")
    lines.append("    const char    *name;")
    lines.append("    short          frames;")
    lines.append("    unsigned char  loop, ntracks;")
    lines.append("    const MCTrack *tracks;")
    lines.append("} MCAnim;")
    lines.append("")

    def bam(deg):
        return int(round(deg / 360.0 * 65536.0)) & 0xFFFF

    for aname, a in ANIMS.items():
        names = [n for n in BONE_ORDER if n in a["tracks"]]
        for bn in names:
            keys = a["tracks"][bn]
            lines.append("static const MCKey mc_k_%s_%s[%d] = {" %
                         (aname, bn, len(keys)))
            for k in keys:
                lines.append("    { %3d, %6d, %6d, %6d, %5d, %5d, %5d }," %
                             (k[0], bam(k[1]), bam(k[2]), bam(k[3]),
                              int(round(k[4] * scale)),
                              int(round(k[5] * scale)),
                              int(round(k[6] * scale))))
            lines.append("};")
        lines.append("static const MCTrack mc_t_%s[%d] = {" % (aname, len(names)))
        for bn in names:
            lines.append("    { %2d, %2d, mc_k_%s_%s }," %
                         (idx[bn], len(a["tracks"][bn]), aname, bn))
        lines.append("};")
        lines.append("")

    lines.append("const MCAnim mc_anims[%d] = {" % len(ANIMS))
    for aname, a in ANIMS.items():
        names = [n for n in BONE_ORDER if n in a["tracks"]]
        lines.append('    { "%s", %3d, %d, %2d, mc_t_%s },' %
                     (aname, a["frames"], a["loop"], len(names), aname))
    lines.append("};")
    lines.append("")
    lines.append("#endif /* MACHINE_CENTAUR_H */")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return path


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scale", type=int, default=8,
                    help="fixed-point units per centimetre for the .h output")
    ap.add_argument("--outdir", default=".")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    build_bones()
    globals()["ANIMS"] = build_anims()

    with bone("hull"):
        build_hull()
    for side in (1, -1):
        for pair in ("front", "rear"):
            build_leg(side, pair)

    # everything from here up is the part of him that used to be a person.
    # Marking the index lets the lean pass find it without touching the knees,
    # which sit above the graft line but belong to the machine.
    human_mark = len(M.v)

    with bone("torso"):
        seam = build_graft()
        build_torso(seam)
        build_spine()
        build_head()
        build_right_arm()
        build_shotgun_arm()

    for i in range(human_mark, len(M.v)):
        x, y, z = M.v[i]
        M.v[i] = (x, y, z + lean_z(y))

    face_q, plate_q, gore_q = build_textures(args.outdir)
    write_obj(args.outdir)
    write_mtl(args.outdir)
    write_header(args.outdir, args.scale, face_q, plate_q, gore_q)

    check_clearance()
    balance_check()
    print("verts: %d   tris: %d   bones: %d   anims: %d"
          % (len(M.v), M.count(), len(BONE_ORDER), len(ANIMS)))
    for g, t in sorted(M.by_material().items(), key=lambda kv: -len(kv[1])):
        print("  %-8s %4d tris" % (g, len(t)))
    for a, d in ANIMS.items():
        print("  anim %-7s %3d frames  %2d tracks  %s"
              % (a, d["frames"], len(d["tracks"]),
                 "loop" if d["loop"] else "one-shot"))
    xs = [p[0] for p in M.v]; ys = [p[1] for p in M.v]; zs = [p[2] for p in M.v]
    print("bbox cm: X %.0f..%.0f  Y %.0f..%.0f  Z %.0f..%.0f"
          % (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))


if __name__ == "__main__":
    main()
