#!/usr/bin/env python3
"""
patrick_horner_gen.py — parametric N64-budget character model.

Subject : Dr. Patrick Horner, PhD — biologist / hardware engineer
Stature : 6'3" (190.5 cm) skeletal; he no longer stands to it
Target  : Nintendo 64, F3DEX2 microcode (32-vertex cache), vertex-colour
          shading + one 32x32 CI4 face texture.

The mesh is built upright, then run through slump(), a Y-keyed deformer that
rounds the thoracic spine, drops and narrows the shoulders, pitches the head
toward the floor, and lets the arms hang dead.  Keeping posture as a separate
pass means the proportions stay editable and you can dial the whole mood from
POSTURE["slump"] — 0.0 gives the original upright build back, and anything in
between is a usable in-between for a cutscene.

Emits:
  patrick_horner.obj      Wavefront OBJ (+ vertex colours, + UVs)
  patrick_horner.mtl      material stub
  patrick_horner.h        F3DEX2 Vtx[] arrays + display lists
  ph_face.png             32x32 indexed face texture (16-colour palette)

Usage:  python3 patrick_horner_gen.py [--scale 8] [--outdir .] [--upright]
"""

import argparse
import math
import os

# --------------------------------------------------------------------------
# palette.  Nothing here is a clean colour any more.
# --------------------------------------------------------------------------
SKIN       = (196, 156, 128)   # sallow
SKIN_DARK  = (152, 116,  92)
STUBBLE    = (124,  98,  82)
HAIR       = ( 24,  22,  28)   # black, unwashed
HAIR_OIL   = ( 44,  42,  52)   # greasy sheen on the crown
COAT       = (188, 190, 178)   # a lab coat that stopped being white
COAT_DARK  = (146, 148, 138)
STAIN      = (128, 116,  86)   # something spilled and never dealt with
SHIRT      = ( 48,  58,  70)
PANTS      = ( 52,  50,  58)
SHOE       = ( 30,  28,  30)
GLASS      = (128, 168, 176)   # safety glasses, shoved up and forgotten
STRAP      = (128,  44,  44)   # ESD wrist strap
EYE_GREEN  = ( 48, 108,  66)   # dulled; lives in the face texture

# --------------------------------------------------------------------------
# proportions — 7.6 heads at 190.5 cm.  Y is up, +Z is forward (facing us),
# origin on the floor midway between the feet.  Units are centimetres.
# These are SKELETAL landmarks; slump() moves the surface off them.
# --------------------------------------------------------------------------
H = 190.5

LANDMARK = {
    "top":      190.5,
    "crown":    185.5,
    "brow":     179.0,
    "eye":      176.5,
    "cheek":    173.0,
    "jaw":      166.0,
    "neck":     157.0,
    "shoulder": 155.0,
    "chest":    140.0,
    "waist":    115.0,
    "hip":       96.0,
    "hem":       78.0,
    "crotch":    92.0,
    "knee":      52.0,
    "ankle":     10.0,
    "floor":      0.0,
    "arm_top":  153.0,
    "elbow":    118.0,
    "cuff":      92.0,
    "wrist":     88.0,
    "fingertip": 72.0,
}

# --------------------------------------------------------------------------
# posture.  slump=0 is the original upright build; 1.0 is where he is now.
# --------------------------------------------------------------------------
POSTURE = {
    "slump":        1.00,
    "head_pitch":   17.0,     # degrees, chin toward the sternum
    "head_pivot":  159.0,     # cm, base of the neck
    "arm_forward":   5.5,     # cm, dead weight hanging in front of the seam
    "arm_inward":    2.0,
    "hand_drop":     0.0,
    "sag_right":     3.0,     # asymmetric droop; symmetry reads as composure
}

# Y-keyed spine curve: (height, dz forward, dy drop, x scale).
# The negative dz at chest height is the rounded upper back; the positive dz
# above it is the shoulders and skull rolling out over his toes.
SPINE = [
    ( 96.0,   0.0,   0.0, 1.000),
    (115.0,  -1.2,  -0.8, 1.000),
    (140.0,  -3.0,  -3.2, 0.985),
    (155.0,   4.0,  -8.5, 0.900),
    (161.0,   8.5,  -9.8, 0.920),
    (166.0,  11.5, -10.6, 0.950),
    (190.5,  13.5, -11.4, 1.000),
]

# The face texture is a planar projection from +Z over this window.  Both the
# UV assignment and the texture painter read it, so they can never drift apart.
FACE_X0, FACE_X1 = -6.2, 6.2       # cm, left/right edge of the face patch
FACE_Y0, FACE_Y1 = 165.5, 179.5    # cm, chin to upper forehead

# --------------------------------------------------------------------------
# mesh buffers
# --------------------------------------------------------------------------
POS, COL, UVS, GRP = [], [], [], []
TRIS = []            # (a, b, c, material, group)
_GROUP = "body"

MAT_SHADE = "shade"  # vertex colour only, no texture bind
MAT_FACE  = "face"   # 32x32 CI4 face texture, modulated by vertex colour


def set_group(name):
    global _GROUP
    _GROUP = name


def vert(p, col, uv=(0.0, 0.0)):
    POS.append([float(p[0]), float(p[1]), float(p[2])])
    COL.append(col)
    UVS.append(uv)
    GRP.append(_GROUP)
    return len(POS) - 1


def _normal(a, b, c):
    ax, ay, az = POS[a]
    bx, by, bz = POS[b]
    cx, cy, cz = POS[c]
    ux, uy, uz = bx - ax, by - ay, bz - az
    vx, vy, vz = cx - ax, cy - ay, cz - az
    return (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)


def tri(a, b, c, ref, mat=MAT_SHADE, group=None):
    """Add a triangle, auto-winding so its normal points away from `ref`."""
    if a == b or b == c or a == c:
        return
    nx, ny, nz = _normal(a, b, c)
    cx = (POS[a][0] + POS[b][0] + POS[c][0]) / 3.0 - ref[0]
    cy = (POS[a][1] + POS[b][1] + POS[c][1]) / 3.0 - ref[1]
    cz = (POS[a][2] + POS[b][2] + POS[c][2]) / 3.0 - ref[2]
    if nx * cx + ny * cy + nz * cz < 0.0:
        a, b, c = a, c, b
    TRIS.append((a, b, c, mat, group or _GROUP))


def quad(a, b, c, d, ref, mat=MAT_SHADE, group=None):
    tri(a, b, c, ref, mat, group)
    tri(a, c, d, ref, mat, group)


def ring(y, rx, rz, n, cols, cx=0.0, cz=0.0, phase=0.0):
    """Horizontal n-gon cross-section.  `cols` is one colour or a list of n."""
    if not isinstance(cols, list):
        cols = [cols] * n
    idx = []
    for i in range(n):
        a = phase + 2.0 * math.pi * i / n
        idx.append(vert((cx + rx * math.cos(a), y, cz + rz * math.sin(a)),
                        cols[i]))
    return idx, (cx, y, cz)


def loft(r0, c0, r1, c1, group=None, mat=MAT_SHADE):
    """Bridge two equal-length rings into a tube segment."""
    ref = ((c0[0] + c1[0]) / 2.0, (c0[1] + c1[1]) / 2.0, (c0[2] + c1[2]) / 2.0)
    n = len(r0)
    for i in range(n):
        j = (i + 1) % n
        quad(r0[i], r0[j], r1[j], r1[i], ref, mat, group)


def cap(r, c, apex, group=None, mat=MAT_SHADE):
    n = len(r)
    for i in range(n):
        tri(r[i], r[(i + 1) % n], apex, c, mat, group)


def box(x0, x1, y0, y1, z0, z1, col, group=None):
    ref = ((x0 + x1) / 2.0, (y0 + y1) / 2.0, (z0 + z1) / 2.0)
    v = [vert((x, y, z), col)
         for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)]
    # index = x*4 + y*2 + z
    faces = [(0, 1, 3, 2), (4, 5, 7, 6), (0, 1, 5, 4),
             (2, 3, 7, 6), (0, 2, 6, 4), (1, 3, 7, 5)]
    for f in faces:
        quad(v[f[0]], v[f[1]], v[f[2]], v[f[3]], ref, MAT_SHADE, group)
    return v


def joint_cap(cx, cy, cz, r, col, group):
    """Octahedral bulge centred on a joint pivot: 6 verts, 8 tris.  Rigid
    segment rigs open a wedge at every bent joint; period N64 models filled
    it with exactly this."""
    set_group(group)
    v = [vert((cx + r, cy, cz), col), vert((cx - r, cy, cz), col),
         vert((cx, cy, cz + r), col), vert((cx, cy, cz - r), col),
         vert((cx, cy + r, cz), col), vert((cx, cy - r, cz), col)]
    ref = (cx, cy, cz)
    for tip in (4, 5):
        for a, b in ((0, 2), (2, 1), (1, 3), (3, 0)):
            tri(v[a], v[b], v[tip], ref, MAT_SHADE, group)


# 8-gon index convention: 0=+X right, 2=+Z front, 4=-X left, 6=-Z back
FRONT8 = (1, 2, 3)
BACK8  = (5, 6, 7)


def head():
    """Head, unwashed hair, nose, safety glasses shoved up and left there."""
    set_group("head")

    def head_cols(front_skin):
        return [HAIR if i not in front_skin else SKIN for i in range(8)]

    jaw, cj = ring(LANDMARK["jaw"], 6.6, 7.2, 8,
                   [HAIR if i in (5, 6, 7) else STUBBLE for i in range(8)],
                   cz=1.0)
    cheek, cc = ring(LANDMARK["cheek"], 8.3, 9.1, 8, head_cols(FRONT8), cz=0.6)
    brow, cb = ring(LANDMARK["brow"], 8.7, 9.4, 8, head_cols(FRONT8), cz=0.0)
    crown, cr = ring(LANDMARK["crown"], 7.6, 8.4, 8,
                     [HAIR_OIL if i in (1, 2, 3) else HAIR for i in range(8)],
                     cz=-0.5)
    apex = vert((0.0, LANDMARK["top"], -0.6), HAIR_OIL)

    fx0, fx1 = FACE_X0, FACE_X1
    fy0, fy1 = FACE_Y0, FACE_Y1
    for r in (jaw, cheek, brow):
        for i in r:
            x, y, _ = POS[i]
            UVS[i] = ((x - fx0) / (fx1 - fx0), 1.0 - (y - fy0) / (fy1 - fy0))

    def seg(r0, c0, r1, c1):
        ref = ((c0[0] + c1[0]) / 2, (c0[1] + c1[1]) / 2, (c0[2] + c1[2]) / 2)
        for i in range(8):
            j = (i + 1) % 8
            m = MAT_FACE if i in (1, 2) else MAT_SHADE   # two front columns
            quad(r0[i], r0[j], r1[j], r1[i], ref, m, "head")

    seg(jaw, cj, cheek, cc)
    seg(cheek, cc, brow, cb)
    loft(brow, cb, crown, cr, "head")
    cap(crown, cr, apex, "head")

    # nose — three tris, just enough silhouette at 320x240
    nose = vert((0.0, 174.2, 10.7), SKIN)
    UVS[nose] = (0.5, 1.0 - (174.2 - fy0) / (fy1 - fy0))
    ref = (0.0, 174.0, 0.0)
    tri(cheek[1], cheek[2], nose, ref, MAT_FACE, "head")
    tri(cheek[2], cheek[3], nose, ref, MAT_FACE, "head")
    tri(brow[2], cheek[2], nose, ref, MAT_FACE, "head")

    # hair he stopped cutting: a forelock hanging into his eyes, and a nape
    # ring that has crept down over the collar
    for x0, x1, drop in ((-6.4, -4.2, 178.8), (0.4, 2.6, 177.8)):
        a = vert((x0, 183.2, 6.6), HAIR)
        b = vert((x1, 183.2, 6.4), HAIR)
        c = vert(((x0 + x1) / 2, drop, 8.6), HAIR)
        tri(a, b, c, (0.0, 182.0, -6.0), MAT_SHADE, "head")
    nape, cn = ring(161.5, 6.4, 6.8, 8,
                    [HAIR if i in (0, 4, 5, 6, 7) else SKIN_DARK
                     for i in range(8)], cz=0.6)
    loft(nape, cn, jaw, cj, "head")

    # safety glasses, pushed up into the hair days ago and never brought down
    gy0, gy1 = 180.6, 183.4
    g = []
    for k, (x, z) in enumerate(((-8.4, 4.6), (-3.2, 9.6),
                                (3.2, 9.6), (8.4, 4.6))):
        t = 2.1 * (k / 3.0 - 0.5) * 2.0          # sitting crooked
        g.append((vert((x, gy0 + t, z), GLASS),
                  vert((x, gy1 + t, z * 0.92), GLASS)))
    gref = (0.0, 182.0, -2.0)
    for i in range(3):
        quad(g[i][0], g[i + 1][0], g[i + 1][1], g[i][1], gref, MAT_SHADE,
             "head")


def neck():
    set_group("neck")
    top, ct = ring(LANDMARK["jaw"] - 1.0, 5.2, 5.4, 6, SKIN, cz=0.4)
    bot, cb = ring(LANDMARK["neck"] - 2.0, 6.1, 6.1, 6, SKIN_DARK)
    loft(top, ct, bot, cb, "neck")


def torso():
    """Torso, coat collar, open lapel, and stains he stopped noticing."""
    set_group("torso")

    def coat_cols(back_shade=True):
        return [COAT_DARK if (back_shade and i in BACK8) else COAT
                for i in range(8)]

    sh, cs = ring(LANDMARK["shoulder"], 23.0, 11.0, 8, coat_cols())
    ch, cc = ring(LANDMARK["chest"], 20.2, 12.6, 8, coat_cols())
    wa, cw = ring(LANDMARK["waist"], 16.6, 10.6, 8, coat_cols())
    hi, chp = ring(LANDMARK["hip"], 18.0, 11.0, 8, coat_cols())
    loft(sh, cs, ch, cc, "torso")
    loft(ch, cc, wa, cw, "torso")
    loft(wa, cw, hi, chp, "torso")

    # collar: without it the neck reads as a giraffe at this head size
    co, ck = ring(161.0, 9.6, 8.6, 8, coat_cols(False))
    loft(sh, cs, co, ck, "torso")
    top = vert((0.0, 162.5, 0.0), COAT_DARK)
    cap(co, ck, top, "torso")

    # A single SHIRT-coloured vertex on the torso ring would gouraud-smear
    # across the whole chest, so the lapel and the stains are separate quads
    # floated just off the surface instead.
    def surf(y, x, out=0.8):
        for (y0, rx0, rz0), (y1, rx1, rz1) in (
                ((LANDMARK["chest"], 20.2, 12.6),
                 (LANDMARK["shoulder"], 23.0, 11.0)),
                ((LANDMARK["waist"], 16.6, 10.6),
                 (LANDMARK["chest"], 20.2, 12.6))):
            if y0 <= y <= y1:
                t = (y - y0) / (y1 - y0)
                rx = rx0 + t * (rx1 - rx0)
                rz = rz0 + t * (rz1 - rz0)
                k = max(0.0, 1.0 - abs(x) / (rx * 0.707) * 0.293)
                return rz * k + out
        return 12.0

    a = vert((-5.6, 157.5, surf(157.5, -5.6)), SHIRT)
    b = vert((5.6, 157.5, surf(157.5, 5.6)), SHIRT)
    c = vert((0.0, 126.0, surf(126.0, 0.0)), SHIRT)
    tri(a, b, c, (0.0, 145.0, -5.0), MAT_SHADE, "torso")

    for (sx, sy, wdt, hgt) in ((-9.0, 133.0, 7.0, 11.0),
                               (6.5, 121.0, 5.0, 7.0)):
        v = [vert((sx + dx, sy + dy, surf(sy + dy, sx + dx, 1.1)), STAIN)
             for dx, dy in ((0, 0), (wdt, -1.5), (wdt * 0.8, -hgt),
                            (-1.0, -hgt * 0.7))]
        quad(v[0], v[1], v[2], v[3], (0.0, 130.0, -5.0), MAT_SHADE, "torso")


def coat_skirt():
    set_group("coat")
    cols = [COAT_DARK if i in BACK8 else COAT for i in range(8)]
    hem = [COAT_DARK if i in BACK8 else STAIN for i in range(8)]
    wa, cw = ring(LANDMARK["waist"], 17.6, 11.6, 8, cols)
    hm, ch = ring(LANDMARK["hem"], 21.0, 14.0, 8, hem)
    loft(wa, cw, hm, ch, "coat")
    bottom = vert((0.0, LANDMARK["hem"] - 0.5, 0.0), COAT_DARK)
    cap(hm, ch, bottom, "coat")


def leg(side):
    set_group("leg")
    s = 1.0 if side > 0 else -1.0
    th, ct = ring(LANDMARK["crotch"], 9.6, 9.6, 6, PANTS, cx=s * 10.4)
    kn, ck = ring(LANDMARK["knee"], 7.1, 7.4, 6, PANTS, cx=s * 10.0)
    an, ca = ring(LANDMARK["ankle"], 5.0, 5.4, 6, PANTS, cx=s * 9.8)
    loft(th, ct, kn, ck, "leg")
    loft(kn, ck, an, ca, "leg")
    joint_cap(s * 10.0, LANDMARK["knee"], 0.0, 7.3, PANTS, "leg")
    set_group("foot")
    box(s * 9.8 - 5.6, s * 9.8 + 5.6, 0.0, 8.4, -8.0, 19.0, SHOE, "foot")


def arm(side):
    set_group("arm")
    s = 1.0 if side > 0 else -1.0
    strap = STRAP if side > 0 else COAT_DARK   # ESD strap on the right wrist
    sh, cs = ring(LANDMARK["arm_top"], 7.6, 7.6, 6, COAT, cx=s * 21.6)
    el, ce = ring(LANDMARK["elbow"], 6.0, 6.0, 6, COAT, cx=s * 23.4)
    cu, cc = ring(LANDMARK["cuff"], 4.9, 4.9, 6, COAT_DARK, cx=s * 24.6)
    wr, cw = ring(LANDMARK["wrist"], 4.4, 4.4, 6, strap, cx=s * 24.8)
    loft(sh, cs, el, ce, "arm")
    loft(el, ce, cu, cc, "arm")
    loft(cu, cc, wr, cw, "arm")
    joint_cap(s * 23.4, LANDMARK["elbow"], 0.0, 6.3, COAT, "arm")
    set_group("hand")
    box(s * 24.8 - 4.4, s * 24.8 + 4.4,
        LANDMARK["fingertip"], LANDMARK["wrist"], -4.2, 4.2, SKIN, "hand")


# ==========================================================================
# posture deformer
# ==========================================================================
def _spine_at(y):
    """Interpolate (dz, dy, sx) off the SPINE curve at height y."""
    if y <= SPINE[0][0]:
        return 0.0, 0.0, 1.0
    for (y0, z0, d0, s0), (y1, z1, d1, s1) in zip(SPINE, SPINE[1:]):
        if y0 <= y <= y1:
            t = (y - y0) / (y1 - y0)
            return z0 + t * (z1 - z0), d0 + t * (d1 - d0), s0 + t * (s1 - s0)
    return SPINE[-1][1], SPINE[-1][2], SPINE[-1][3]


def slump_point(x, y, z, group, k=None):
    """Apply the posture transform to a single upright-space point.

    Exposed so the rig can push joint pivots through exactly the same maths
    as the vertices.  A skeleton built off the upright landmarks and a mesh
    built off the slumped ones would disagree by centimetres at the neck.
    """
    if k is None:
        k = POSTURE["slump"]
    if k <= 0.0:
        return [x, y, z]

    sdz, sdy, ssx = _spine_at(LANDMARK["arm_top"])
    seam_pull = 23.0 * (1.0 - ssx)          # how far the shoulder narrowed

    if group in ("arm", "hand"):
        side = 1.0 if x >= 0.0 else -1.0
        x -= side * (seam_pull + POSTURE["arm_inward"]) * k
        y += sdy * k
        z += sdz * k
        slack = max(0.0, (LANDMARK["arm_top"] - y) / 80.0)
        z += POSTURE["arm_forward"] * k * slack
        if group == "hand":
            y -= POSTURE["hand_drop"] * k
            z += POSTURE["arm_forward"] * k * 0.35
        if side > 0:                        # the right side droops further
            y -= POSTURE["sag_right"] * k * 0.6
    elif group in ("leg", "foot"):
        pass                                # the stance itself is unchanged
    else:
        dz, dy, sx = _spine_at(y)
        x *= 1.0 + (sx - 1.0) * k
        y += dy * k
        z += dz * k
        if group in ("torso", "coat") and x > 0.0 and y > LANDMARK["chest"]:
            y -= POSTURE["sag_right"] * k * (y - LANDMARK["chest"]) / 20.0

    # head and neck pitch forward about the base of the neck, weighted in so
    # the collar end of the neck stays welded to the torso
    if group in ("head", "neck"):
        th = math.radians(POSTURE["head_pitch"] * k)
        pdz, pdy, _ = _spine_at(POSTURE["head_pivot"])
        py = POSTURE["head_pivot"] + pdy * k
        pz = pdz * k
        dy, dz = y - py, z - pz
        if dy >= -6.0:
            w = min(1.0, max(0.0, (dy + 6.0) / 10.0))
            ct, st = math.cos(th * w), math.sin(th * w)
            y, z = py + dy * ct - dz * st, pz + dz * ct + dy * st
    return [x, y, z]


def slump(amount=None):
    """Y-keyed posture pass.  Run once, after build, before export."""
    for i, (x, y, z) in enumerate(POS):
        POS[i] = slump_point(x, y, z, GRP[i], amount)


def build(upright=False):
    head()
    neck()
    torso()
    coat_skirt()
    for side in (1, -1):
        arm(side)
        leg(side)
    if not upright:
        slump()


# --------------------------------------------------------------------------
# 32x32 CI4 face texture — 16-colour palette, 512 bytes + 32-byte TLUT
# --------------------------------------------------------------------------
def write_face_texture(path):
    from PIL import Image
    W = 32
    pal = [SKIN,               # 0 base
           SKIN_DARK,          # 1 shadow
           HAIR,               # 2
           EYE_GREEN,          # 3 dulled iris
           (192, 190, 176),    # 4 dingy sclera
           STUBBLE,            # 5
           (108, 70, 62),      # 6 mouth
           (118, 88, 78),      # 7 the bruise under each eye
           (82, 58, 50),       # 8 deep hollow
           HAIR_OIL,           # 9 greasy hair highlight
           (16, 16, 18),       # 10 pupil
           (188, 162, 152),    # 11 bloodshot corner
           (210, 176, 150),    # 12 oily highlight
           (40, 30, 26),       # 13 near-black
           (78, 50, 44),       # 14 lip line
           (255, 0, 255)]      # 15 unused
    px = [[0] * W for _ in range(W)]

    def rect(x0, y0, x1, y1, c):
        for y in range(max(0, y0), min(W, y1)):
            for x in range(max(0, x0), min(W, x1)):
                px[y][x] = c

    # rows map linearly from FACE_Y1 (row 0) down to FACE_Y0 (row 32),
    # i.e. 0.4375 cm per row.  At this size restraint is everything: a
    # two-pixel brow reads as a brow, a four-pixel brow reads as a mask.
    rect(0, 0, W, W, 0)                       # base skin, already sallow
    rect(0, 0, W, 3, 2)                       # hairline
    rect(0, 0, 3, 12, 2)                      # grown down past the ears
    rect(29, 0, 32, 12, 2)
    for x0 in (6, 13, 21, 26):                # a few strands he never pushes back
        rect(x0, 2, x0 + 2, 6, 2)
    rect(4, 3, 6, 4, 9)                       # greasy highlight
    rect(13, 2, 19, 3, 1)                     # a brow furrow that never leaves

    # eyebrows: one pixel tall, inner ends lifted.  The grief brow does more
    # work here than anything else on the texture.
    rect(5, 6, 9, 7, 2)
    rect(9, 5, 13, 6, 2)
    rect(19, 5, 23, 6, 2)
    rect(23, 6, 27, 7, 2)

    rect(5, 8, 12, 11, 4)                     # eye openings
    rect(20, 8, 27, 11, 4)
    rect(5, 8, 12, 9, 1)                      # lids at half mast
    rect(20, 8, 27, 9, 1)
    rect(7, 9, 10, 11, 3)                     # dull irises
    rect(22, 9, 25, 11, 3)
    rect(8, 9, 10, 11, 10)                    # pupils
    rect(22, 9, 24, 11, 10)
    rect(5, 10, 6, 11, 11)                    # red inner corners
    rect(26, 10, 27, 11, 11)
    rect(5, 11, 12, 13, 7)                    # the bruise under each eye
    rect(20, 11, 27, 13, 7)

    rect(15, 11, 17, 15, 12)                  # nose, gone shiny
    rect(14, 15, 18, 16, 1)                   # nostril shadow
    rect(4, 16, 6, 24, 1)                     # cheeks fallen in
    rect(26, 16, 28, 24, 1)

    rect(12, 21, 20, 22, 6)                   # mouth
    rect(10, 22, 12, 23, 14)                  # corners pulled down
    rect(20, 22, 22, 23, 14)
    rect(13, 20, 19, 21, 1)

    for y in range(21, 30):                   # four days of stubble, sparse
        for x in range(5, 27):
            if (x * 5 + y * 3) % 6 == 0 and not (10 <= x < 22 and 20 <= y < 23):
                px[y][x] = 5

    img = Image.new("P", (W, W))
    flat = []
    for c in pal:
        flat.extend(c)
    img.putpalette(flat)
    img.putdata([px[y][x] for y in range(W) for x in range(W)])
    img.save(path)

    # raw CI4 + RGBA5551 TLUT, ready to #include if you skip mksprite
    ci4 = bytearray()
    for y in range(W):
        for x in range(0, W, 2):
            ci4.append((px[y][x] << 4) | px[y][x + 1])
    tlut = bytearray()
    for r, g, b in pal:
        v = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1
        tlut += bytes(((v >> 8) & 0xFF, v & 0xFF))
    return bytes(ci4), bytes(tlut)


# --------------------------------------------------------------------------
# exporters
# --------------------------------------------------------------------------
def write_obj(path, mtlname):
    with open(path, "w") as f:
        f.write("# Dr. Patrick Horner - N64 budget character model\n")
        f.write("# %d verts / %d tris\n" % (len(POS), len(TRIS)))
        f.write("mtllib %s\no patrick_horner\n" % mtlname)
        for p, c in zip(POS, COL):
            f.write("v %.3f %.3f %.3f %.4f %.4f %.4f\n"
                    % (p[0], p[1], p[2], c[0] / 255, c[1] / 255, c[2] / 255))
        for u, v in UVS:
            f.write("vt %.5f %.5f\n" % (u, 1.0 - v))
        cur = None
        for a, b, c, mat, _ in sorted(TRIS, key=lambda t: t[3]):
            if mat != cur:
                f.write("usemtl %s\n" % mat)
                cur = mat
            f.write("f %d/%d %d/%d %d/%d\n"
                    % (a + 1, a + 1, b + 1, b + 1, c + 1, c + 1))


def write_mtl(path):
    with open(path, "w") as f:
        f.write("newmtl shade\nKd 1 1 1\nillum 1\n\n")
        f.write("newmtl face\nKd 1 1 1\nmap_Kd ph_face.png\nillum 1\n")


def batch(tris, cache=32):
    """Split a triangle list into F3DEX2 vertex-cache loads (<=32 verts)."""
    out, verts, remap, local = [], [], {}, []
    for a, b, c in tris:
        need = [v for v in dict.fromkeys((a, b, c)) if v not in remap]
        if len(verts) + len(need) > cache:
            out.append((verts, local))
            verts, remap, local = [], {}, []
            need = list(dict.fromkeys((a, b, c)))
        for v in need:
            remap[v] = len(verts)
            verts.append(v)
        local.append((remap[a], remap[b], remap[c]))
    if local:
        out.append((verts, local))
    return out


def write_header(path, scale):
    groups = {}
    for a, b, c, mat, grp in TRIS:
        groups.setdefault(mat, []).append((a, b, c))

    def s16(v):
        return max(-32768, min(32767, int(round(v * scale))))

    top = max(p[1] for p in POS)
    lines = []
    w = lines.append
    w("/* patrick_horner.h - generated by patrick_horner_gen.py, do not edit")
    w(" *")
    w(" * Dr. Patrick Horner, PhD  |  %d verts / %d tris"
      % (len(POS), len(TRIS)))
    w(" * Skeletal stature 190.5 cm; slumped standing height %.1f cm." % top)
    w(" * F3DEX2, 32-vertex cache, s16 model space at %d units/cm." % scale)
    w(" * Shading: gouraud vertex colour (G_LIGHTING off).")
    w(" * Face texture: ph_face  32x32 CI4 + 16-entry TLUT (512 B + 32 B).")
    w(" *")
    w(" * Dropping him into a baked room wants a shade tint, not a light:")
    w(" *   gDPSetPrimColor(...room tint...) with a PRIMITIVE*SHADE combine.")
    w(" */")
    w("#ifndef PATRICK_HORNER_H")
    w("#define PATRICK_HORNER_H")
    w("")
    w("#define PH_MODEL_SCALE      %d       /* s16 units per centimetre */"
      % scale)
    w("#define PH_HEIGHT_CM        %.1ff    /* as he actually stands */" % top)
    w("#define PH_EYE_HEIGHT       %d       /* s16, for camera framing */"
      % s16(top - 16.0))
    w("#define PH_VERTEX_COUNT     %d" % len(POS))
    w("#define PH_TRIANGLE_COUNT   %d" % len(TRIS))
    w("")

    dl_bodies = {}
    for mat in ("shade", "face"):
        if mat not in groups:
            continue
        loads = batch(groups[mat])
        vname = "ph_vtx_%s" % mat
        flat = []
        for vlist, _ in loads:
            flat.extend(vlist)
        w("static const Vtx %s[] = {" % vname)
        for v in flat:
            x, y, z = POS[v]
            u, t = UVS[v]
            r, g, b = COL[v]
            w("    {{{%6d,%6d,%6d}, 0, {%5d,%5d}, {%3d,%3d,%3d,255}}},"
              % (s16(x), s16(y), s16(z),
                 int(round(u * 31 * 32)), int(round(t * 31 * 32)), r, g, b))
        w("};")
        w("")
        body, base = [], 0
        for vlist, tl in loads:
            body.append("    gsSPVertex(&%s[%d], %d, 0),"
                        % (vname, base, len(vlist)))
            i = 0
            while i + 1 < len(tl):
                a = tl[i]
                b = tl[i + 1]
                body.append("    gsSP2Triangles(%d,%d,%d, 0, %d,%d,%d, 0),"
                            % (a[0], a[1], a[2], b[0], b[1], b[2]))
                i += 2
            if i < len(tl):
                body.append("    gsSP1Triangle(%d,%d,%d, 0)," % tl[i])
            base += len(vlist)
        dl_bodies[mat] = body

    w("static const Gfx ph_dl_body[] = {")
    w("    gsDPPipeSync(),")
    w("    gsSPClearGeometryMode(G_LIGHTING | G_TEXTURE_GEN),")
    w("    gsSPSetGeometryMode(G_SHADE | G_SHADING_SMOOTH | G_CULL_BACK"
      " | G_ZBUFFER),")
    w("    gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE),")
    w("    gsSPTexture(0, 0, 0, 0, G_OFF),")
    for l in dl_bodies.get("shade", []):
        w(l)
    w("    gsSPEndDisplayList(),")
    w("};")
    w("")
    w("/* Bind the 32x32 CI4 face + TLUT before calling ph_dl_face. */")
    w("static const Gfx ph_dl_face[] = {")
    w("    gsDPPipeSync(),")
    w("    gsDPSetCombineMode(G_CC_MODULATEIDECALA, G_CC_MODULATEIDECALA),")
    w("    gsSPTexture(0x8000, 0x8000, 0, G_TX_RENDERTILE, G_ON),")
    for l in dl_bodies.get("face", []):
        w(l)
    w("    gsSPTexture(0, 0, 0, 0, G_OFF),")
    w("    gsSPEndDisplayList(),")
    w("};")
    w("")
    w("static const Gfx ph_dl_draw[] = {")
    w("    gsSPDisplayList(ph_dl_body),")
    w("    gsSPDisplayList(ph_dl_face),")
    w("    gsSPEndDisplayList(),")
    w("};")
    w("")
    w("#endif /* PATRICK_HORNER_H */")

    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scale", type=int, default=8,
                    help="s16 model units per centimetre (default 8)")
    ap.add_argument("--outdir", default=".")
    ap.add_argument("--upright", action="store_true",
                    help="skip the posture pass and export him standing tall")
    a = ap.parse_args()

    build(upright=a.upright)
    os.makedirs(a.outdir, exist_ok=True)
    p = lambda n: os.path.join(a.outdir, n)

    write_obj(p("patrick_horner.obj"), "patrick_horner.mtl")
    write_mtl(p("patrick_horner.mtl"))
    ci4, tlut = write_face_texture(p("ph_face.png"))
    write_header(p("patrick_horner.h"), a.scale)

    grp = {}
    for *_, g in TRIS:
        grp[g] = grp.get(g, 0) + 1
    print("verts %d   tris %d   standing height %.1f cm   face %d B + %d B"
          % (len(POS), len(TRIS), max(q[1] for q in POS), len(ci4), len(tlut)))
    for k in sorted(grp):
        print("   %-6s %4d tris" % (k, grp[k]))


if __name__ == "__main__":
    main()
