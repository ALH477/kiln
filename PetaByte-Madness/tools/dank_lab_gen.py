#!/usr/bin/env python3
"""
dank_lab_gen.py — parametric N64-budget interior: Horner's wet lab.

Three zones, hard-partitioned so the console never draws all of them:

  room   5.6 x 4.6 x 2.5 m   the lab itself
  hall   3.4 x 1.2 x 2.05 m  a crawl of a corridor
  dock   0.8 m trunk         the submersible hatch at the end of it

Nothing in here was installed.  It was carried down, set on whatever was
handy, and wired up in an afternoon: the scanner sits on its shipping
pallet still strapped, the bench is a folding table with a plywood
extension on cinder blocks, the lights are clamp lamps on drop cords, and
the mains run is a daisy chain of extension leads gaffered to the deck.
That reads as "rapid" far better than making things look broken does —
broken says old, improvised says they arrived in a hurry.

Design notes for the console:
  * Room shell is subdivided so the baked vertex lighting has somewhere to
    live.  One quad per wall would give you four corner colours and a
    hideous gradient.
  * Lighting is baked into vertex colours by a point-light pass (bake()).
    G_LIGHTING stays OFF at runtime - the RSP does nothing but transform.
  * Fog carries the "underwater" read more than any texture does.
  * Four 32x32 CI4 textures, 512 B each.  Only 2 KB of TMEM is available
    for texel data once TLUTs occupy the upper half, so these are loaded
    per material, not all at once.
  * Every triangle carries a zone tag, and the header emits one display
    list per zone.  Draw the zone you are standing in plus whatever the
    doorway portal can see; never chain lab_dl_all in a shipping build.

Emits:
  dank_lab.obj / .mtl          geometry for Blender
  dank_lab.h                   F3DEX2 Vtx[] + per-zone/per-material DLs
  lab_wall.png  lab_floor.png  lab_screen.png  lab_caustic.png

Usage:  python3 dank_lab_gen.py [--scale 8] [--outdir .]
"""

import argparse
import math
import os

# --------------------------------------------------------------------------
# envelope (cm).  Origin on the floor at room centre, Y up, +Z is the
# porthole wall, -Z is the scanner wall, -X is the way out.
# --------------------------------------------------------------------------
RX, RZ, RY = 280.0, 230.0, 250.0

DOOR_Z0, DOOR_Z1 = 12.0, 128.0        # doorway in the -X wall
DOOR_Y = 198.0
HALL_X = -620.0                       # far end of the corridor
HALL_Y = 205.0
TRUNK_X = -700.0                      # submersible hatch face
COLLAR_Z, COLLAR_Y = 70.0, 98.0       # centre of the docking collar
COLLAR_R, COLLAR_OUT = 52.0, 76.0

# --------------------------------------------------------------------------
# fixtures the RUNTIME needs to know about (cm).
#
# These were locals inside scanner() and workstations(), which meant nothing
# outside this file could read them — so PetaByte-Madness/src/pm_lab.h and
# pm_intake.c restated them by hand, with the arithmetic written out in
# comments ("STOOL_X (178) * 0.64"). That is exactly the duplication the
# project's modelling rule forbids: never type a dimension into C that
# geometry already defines. Lifted here so --emit-header can publish them and
# the C side can derive instead of guess.
# --------------------------------------------------------------------------
SCANNER_CX, SCANNER_CY = -70.0, 108.0   # 14 cm of pallet under it
SCANNER_ZFACE, SCANNER_ZBACK = -55.0, -222.0
SCANNER_ROUT, SCANNER_RBORE = 92.0, 34.0

STOOL_X, STOOL_Z = 178.0, 34.0          # the desk seat pm_intake.c sits him on

# --------------------------------------------------------------------------
# albedo palette — unlit surface colours.  bake() darkens them.
# --------------------------------------------------------------------------
STEEL      = (108, 116, 118)
STEEL_DARK = ( 70,  76,  80)
RUST       = (104,  72,  48)
GRATE      = ( 82,  88,  90)
PLASTIC    = (168, 166, 154)   # scanner shell, dirty cream
PLASTIC_D  = (124, 122, 112)
BLACKPLAST = ( 42,  44,  46)
GLASS_DK   = ( 30,  46,  52)
CHROME     = (150, 158, 162)
LIT_WHITE  = (244, 248, 246)   # albedo for TEXTURED surfaces: MODULATE means
                               # the shade channel should be pure lighting
PALLET     = (128, 102,  66)   # raw pine, still stencilled
CRATE      = ( 96,  90,  76)
PELICAN    = ( 52,  56,  58)
TARP       = (108, 112, 100)   # polythene sheeting, taped up
CINDER     = (118, 116, 108)
PLY        = (152, 122,  78)
CABLE      = ( 46,  46,  52)
GAFFER     = ( 62,  62,  64)
SUB_HULL   = (176, 132,  36)   # the one warm colour down here

# emissive colours bypass the lighting bake entirely
E_TUBE     = (188, 214, 198)   # clamp lamp
E_BORE     = ( 70, 210, 235)   # scanner bore ring
E_SCREEN   = ( 74, 226, 150)   # CRT phosphor
E_TANK     = (104, 236, 132)   # specimen culture
E_LED_R    = (236, 100,  60)
E_LED_A    = (238, 186,  70)
E_PORT     = ( 34, 104, 118)   # silt-lit water beyond the glass
E_SUB      = (248, 186,  96)   # the submersible's cabin light

# --------------------------------------------------------------------------
# baked lighting rig.  (x, y, z, colour, radius_cm, intensity)
# One fixture is deliberately dead - a room where every lamp works does not
# look like a room somebody threw together.
# --------------------------------------------------------------------------
AMBIENT = (0.060, 0.082, 0.090)     # dank teal, never fully black

LIGHTS = [
    (-96.0, 214.0,  -46.0, E_TUBE,   330.0, 0.95),   # clamp lamp over the bore
    (118.0, 218.0,   96.0, E_TUBE,   300.0, 0.72),   # clamp lamp over the desk
    (-70.0, 100.0,  -50.0, E_BORE,   265.0, 1.05),   # scanner bore
    (232.0, 112.0,   20.0, E_SCREEN, 200.0, 0.80),   # CRT bank
    (-180.0, 130.0, 190.0, E_TANK,   175.0, 0.78),   # specimen tanks
    (-120.0, 130.0, 190.0, E_TANK,   165.0, 0.66),
    (-120.0, 175.0, 222.0, E_PORT,   265.0, 0.72),   # porthole spill
    (60.0,  175.0,  222.0, E_PORT,   265.0, 0.72),
    (242.0, 152.0,  180.0, E_LED_R,  150.0, 0.50),   # rack LEDs
    (-330.0, 186.0,  70.0, E_TUBE,   210.0, 0.60),   # hall lamp, still lit
    (-560.0, 186.0,  70.0, E_TUBE,   190.0, 0.34),   # hall lamp, half dead
    (-660.0, COLLAR_Y, COLLAR_Z, E_SUB, 300.0, 1.05),  # the sub, warm
]

# --------------------------------------------------------------------------
# mesh buffers
# --------------------------------------------------------------------------
POS, COL, UVS = [], [], []
TRIS = []                    # (a, b, c, material, group, zone)
EMISSIVE = set()             # vertex indices the bake must not touch
_ZONE = "room"

MAT_SHADE   = "shade"        # vertex colour only
MAT_WALL    = "wall"         # lab_wall.png,    G_TX_WRAP
MAT_FLOOR   = "floor"        # lab_floor.png,   G_TX_WRAP
MAT_SCREEN  = "screen"       # lab_screen.png,  emissive, G_TX_CLAMP
MAT_CAUSTIC = "caustic"      # lab_caustic.png, 1-bit alpha decal, scrolled

ZONES = ("room", "hall", "dock")
TEX_W = 32                   # every texture in this set is 32x32


def set_zone(z):
    global _ZONE
    _ZONE = z


def vert(p, col, uv=(0.0, 0.0), emissive=False):
    POS.append((float(p[0]), float(p[1]), float(p[2])))
    COL.append(tuple(col))
    UVS.append(uv)
    i = len(POS) - 1
    if emissive:
        EMISSIVE.add(i)
    return i


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _face_normal(a, b, c):
    ax, ay, az = POS[a]
    u = (POS[b][0] - ax, POS[b][1] - ay, POS[b][2] - az)
    v = (POS[c][0] - ax, POS[c][1] - ay, POS[c][2] - az)
    return _cross(u, v)


def tri(a, b, c, ref, mat=MAT_SHADE, group="misc"):
    """Add a triangle wound so its normal points AWAY from `ref`.

    For interior surfaces pass a `ref` outside the room, so the face ends up
    looking inward and survives G_CULL_BACK."""
    if a == b or b == c or a == c:
        return
    n = _face_normal(a, b, c)
    cen = tuple((POS[a][k] + POS[b][k] + POS[c][k]) / 3.0 - ref[k]
                for k in range(3))
    if sum(n[k] * cen[k] for k in range(3)) < 0.0:
        b, c = c, b
    TRIS.append((a, b, c, mat, group, _ZONE))


def quad(a, b, c, d, ref, mat=MAT_SHADE, group="misc"):
    tri(a, b, c, ref, mat, group)
    tri(a, c, d, ref, mat, group)


def grid(org, du, dv, nu, nv, ref, col, mat, group, tile=120.0, emissive=False):
    """Subdivided planar panel.  `tile` = cm per texture repeat."""
    lu = math.sqrt(sum(k * k for k in du))
    lv = math.sqrt(sum(k * k for k in dv))
    ix = [[0] * (nv + 1) for _ in range(nu + 1)]
    for i in range(nu + 1):
        for j in range(nv + 1):
            s, t = i / nu, j / nv
            p = tuple(org[k] + du[k] * s + dv[k] * t for k in range(3))
            ix[i][j] = vert(p, col, (s * lu / tile, t * lv / tile), emissive)
    for i in range(nu):
        for j in range(nv):
            quad(ix[i][j], ix[i + 1][j], ix[i + 1][j + 1], ix[i][j + 1],
                 ref, mat, group)
    return ix


def box(x0, x1, y0, y1, z0, z1, col, group="misc", mat=MAT_SHADE,
        emissive=False, faces="xyzXYZ"):
    ref = ((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2)
    v = [vert((x, y, z), col, (0, 0), emissive)
         for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)]
    plan = {"x": (0, 1, 3, 2), "X": (4, 5, 7, 6), "y": (0, 1, 5, 4),
            "Y": (2, 3, 7, 6), "z": (0, 2, 6, 4), "Z": (1, 3, 7, 5)}
    for key in faces:
        f = plan[key]
        quad(v[f[0]], v[f[1]], v[f[2]], v[f[3]], ref, mat, group)
    return v


def ring(axis, along, r, n, col, cu=0.0, cv=0.0, phase=0.0, group="misc",
         emissive=False):
    """n-gon perpendicular to `axis` ('x'|'y'|'z') at coordinate `along`."""
    idx = []
    for i in range(n):
        a = phase + 2.0 * math.pi * i / n
        u, v = cu + r * math.cos(a), cv + r * math.sin(a)
        if axis == "z":
            p = (u, v, along)
        elif axis == "y":
            p = (u, along, v)
        else:
            p = (along, u, v)
        idx.append(vert(p, col, (0, 0), emissive))
    c = {"z": (cu, cv, along), "y": (cu, along, cv), "x": (along, cu, cv)}[axis]
    return idx, c


def loft(r0, c0, r1, c1, group="misc", mat=MAT_SHADE):
    ref = tuple((c0[k] + c1[k]) / 2.0 for k in range(3))
    n = len(r0)
    for i in range(n):
        j = (i + 1) % n
        quad(r0[i], r0[j], r1[j], r1[i], ref, mat, group)


def annulus(outer, inner, ref, group="misc", mat=MAT_SHADE):
    """Flat washer between two coaxial rings of equal vertex count."""
    n = len(outer)
    for i in range(n):
        j = (i + 1) % n
        quad(outer[i], outer[j], inner[j], inner[i], ref, mat, group)


def disc(r, c, apex, ref, group="misc", mat=MAT_SHADE):
    n = len(r)
    for i in range(n):
        tri(r[i], r[(i + 1) % n], apex, ref, mat, group)


def cable(pts, r, col, group="cable"):
    """Slack cable as a 3-gon tube through a polyline.  A triangular
    cross-section costs 6 tris per span instead of 8 and is indis-
    tinguishable from a round one at this diameter."""
    ang = (1.571, 3.665, 5.760)
    prev = None
    for (x, y, z) in pts:
        cur = [vert((x + r * math.cos(a), y + r * math.sin(a) * 0.85, z), col)
               for a in ang]
        if prev is not None:
            px, py, pz = prev[1]
            ref = ((x + px) / 2, (y + py) / 2 + 40.0, (z + pz) / 2)
            for i in range(3):
                j = (i + 1) % 3
                quad(prev[0][i], prev[0][j], cur[j], cur[i], ref,
                     MAT_SHADE, group)
        prev = (cur, (x, y, z))


# ==========================================================================
# the room
# ==========================================================================
def shell():
    """Floor, ceiling, walls.  Normals point inward.  -X wall has a doorway."""
    grid((-RX, 0.0, -RZ), (2 * RX, 0, 0), (0, 0, 2 * RZ), 5, 4,
         (0.0, -400.0, 0.0), LIT_WHITE, MAT_FLOOR, "floor", tile=140.0)
    grid((-RX, RY, -RZ), (2 * RX, 0, 0), (0, 0, 2 * RZ), 4, 3,
         (0.0, 700.0, 0.0), LIT_WHITE, MAT_WALL, "ceiling", tile=160.0)
    grid((RX, 0.0, -RZ), (0, 0, 2 * RZ), (0, RY, 0), 4, 3,
         (700.0, 120.0, 0.0), LIT_WHITE, MAT_WALL, "wall_xp", tile=125.0)
    grid((-RX, 0.0, -RZ), (2 * RX, 0, 0), (0, RY, 0), 4, 3,
         (0.0, 120.0, -700.0), LIT_WHITE, MAT_WALL, "wall_zm", tile=125.0)
    grid((-RX, 0.0, RZ), (2 * RX, 0, 0), (0, RY, 0), 4, 3,
         (0.0, 120.0, 700.0), LIT_WHITE, MAT_WALL, "wall_zp", tile=125.0)

    # -X wall, built as four panels around the corridor doorway
    ref = (-700.0, 120.0, 0.0)
    grid((-RX, 0.0, -RZ), (0, 0, RZ + DOOR_Z0), (0, RY, 0), 2, 3,
         ref, LIT_WHITE, MAT_WALL, "wall_xm", tile=125.0)
    grid((-RX, 0.0, DOOR_Z1), (0, 0, RZ - DOOR_Z1), (0, RY, 0), 1, 3,
         ref, LIT_WHITE, MAT_WALL, "wall_xm", tile=125.0)
    grid((-RX, DOOR_Y, DOOR_Z0), (0, 0, DOOR_Z1 - DOOR_Z0),
         (0, RY - DOOR_Y, 0), 1, 1, ref, LIT_WHITE, MAT_WALL, "wall_xm",
         tile=125.0)


def doorframe():
    """A hatch that was cut, dogged open and never closed again."""
    x = -RX
    box(x - 6.0, x + 5.0, 0.0, DOOR_Y + 8.0, DOOR_Z0 - 9.0, DOOR_Z0,
        STEEL_DARK, "door")
    box(x - 6.0, x + 5.0, 0.0, DOOR_Y + 8.0, DOOR_Z1, DOOR_Z1 + 9.0,
        STEEL_DARK, "door")
    box(x - 6.0, x + 5.0, DOOR_Y, DOOR_Y + 8.0, DOOR_Z0, DOOR_Z1,
        STEEL_DARK, "door")
    # the hatch itself, swung flat against the room wall and wedged
    box(x + 5.0, x + 12.0, 14.0, 176.0, DOOR_Z1 + 12.0, DOOR_Z1 + 104.0,
        STEEL, "door")
    o, co = ring("x", x + 16.0, 22.0, 8, CHROME, 96.0, DOOR_Z1 + 58.0,
                 group="door")
    i, ci = ring("x", x + 16.0, 14.0, 8, CHROME, 96.0, DOOR_Z1 + 58.0,
                 group="door")
    annulus(o, i, (x - 60.0, 96.0, DOOR_Z1 + 58.0), "door")
    # hand-lettered placard, because nobody made signage for this place
    box(x + 5.0, x + 7.0, 202.0, 218.0, DOOR_Z0 + 14.0, DOOR_Z0 + 62.0,
        E_LED_A, "door", emissive=True, faces="X")


def porthole(cx, cy, r=38.0):
    """Octagonal port in the +Z wall: recessed glass, murk beyond, bezel."""
    z_glass, z_bezel = RZ - 3.0, RZ - 11.0
    out, co = ring("z", z_bezel, r + 11.0, 8, CHROME, cx, cy, group="port")
    inn, ci = ring("z", z_bezel, r, 8, STEEL_DARK, cx, cy, group="port")
    annulus(out, inn, (cx, cy, RZ + 60.0), "port")
    gl, cg = ring("z", z_glass, r, 8, E_PORT, cx, cy, group="port",
                  emissive=True)
    loft(inn, ci, gl, cg, "port")
    core = vert((cx, cy, z_glass + 1.0),
                tuple(min(255, int(c * 1.35)) for c in E_PORT), emissive=True)
    disc(gl, cg, core, (cx, cy, RZ + 60.0), "port")
    for i in range(6):
        a = math.pi * (0.25 + i / 6.0 * 2.0)
        bx, by = cx + (r + 6.5) * math.cos(a), cy + (r + 6.5) * math.sin(a)
        box(bx - 2.4, bx + 2.4, by - 2.4, by + 2.4, z_bezel - 2.5, z_bezel,
            CHROME, "port", faces="z")


def scanner():
    """Bore scanner still on its shipping pallet, still strapped down."""
    cx, cy = SCANNER_CX, SCANNER_CY
    z_face, z_back = SCANNER_ZFACE, SCANNER_ZBACK
    R_OUT, R_BORE = SCANNER_ROUT, SCANNER_RBORE
    N = 12

    fo, cfo = ring("z", z_face, R_OUT, N, PLASTIC, cx, cy, group="mri")
    ga, cga = ring("z", -128.0, R_OUT, N, PLASTIC, cx, cy, group="mri")
    gb, cgb = ring("z", -140.0, R_OUT - 5.0, N, BLACKPLAST, cx, cy, group="mri")
    gc, cgc = ring("z", -152.0, R_OUT, N, PLASTIC_D, cx, cy, group="mri")
    bo, cbo = ring("z", z_back, R_OUT, N, PLASTIC_D, cx, cy, group="mri")
    loft(fo, cfo, ga, cga, "mri")
    loft(ga, cga, gb, cgb, "mri")
    loft(gb, cgb, gc, cgc, "mri")
    loft(gc, cgc, bo, cbo, "mri")

    fi, cfi = ring("z", z_face, R_BORE, N, PLASTIC_D, cx, cy, group="mri")
    annulus(fo, fi, (cx, cy, z_back - 200.0), "mri")
    bi, cbi = ring("z", z_back + 6.0, R_BORE, N, BLACKPLAST, cx, cy,
                   group="mri")
    loft(fi, cfi, bi, cbi, "mri")
    cap = vert((cx, cy, z_back + 6.0), BLACKPLAST)
    disc(bi, cbi, cap, (cx, cy, z_face + 200.0), "mri")

    ea, cea = ring("z", z_face + 2.0, R_BORE + 15.0, N, E_BORE, cx, cy,
                   group="mri", emissive=True)
    eb, ceb = ring("z", z_face + 2.0, R_BORE + 2.0, N, E_BORE, cx, cy,
                   group="mri", emissive=True)
    annulus(ea, eb, (cx, cy, z_back - 200.0), "mri")
    box(cx - 46.0, cx + 46.0, cy + 58.0, cy + 80.0, z_face, z_face + 3.0,
        BLACKPLAST, "mri", faces="Z")
    box(cx - 34.0, cx + 34.0, cy + 64.0, cy + 71.0, z_face + 3.0, z_face + 4.5,
        E_BORE, "mri", emissive=True, faces="Z")

    # the pallet, the straps, and the panel they took off and leaned on it
    box(cx - 104.0, cx + 104.0, 0.0, 14.0, z_back - 6.0, z_face + 6.0,
        PALLET, "pallet")
    for zs in (-90.0, -190.0):
        box(cx - 96.0, cx + 96.0, 14.0, 18.0, zs - 3.0, zs + 3.0, PALLET,
            "pallet", faces="yY")
        box(cx - 97.0, cx - 91.0, 14.0, 96.0, zs - 3.5, zs + 3.5, GAFFER,
            "pallet", faces="xzZ")                          # ratchet strap
        box(cx + 91.0, cx + 97.0, 14.0, 96.0, zs - 3.5, zs + 3.5, GAFFER,
            "pallet", faces="XzZ")
    box(cx + 96.0, cx + 100.0, 16.0, 118.0, -120.0, -30.0, PLASTIC_D,
        "pallet")                                            # removed panel
    # loom spilling out of the hole where the panel used to be
    cable([(cx + 92.0, 108.0, -70.0), (cx + 118.0, 74.0, -40.0),
           (cx + 150.0, 28.0, 10.0), (cx + 186.0, 14.0, 60.0)], 5.0, CABLE,
          "cable")

    # The slab is its own group so the animation system can slide it into
    # the bore independently of the shell.
    box(cx - 30.0, cx + 30.0, 78.0, 88.0, -60.0, 120.0, PLASTIC, "table")
    box(cx - 8.0, cx + 8.0, 18.0, 78.0, 88.0, 104.0, CHROME, "table_base")


def debris():
    """Two severed limbs off a machine centaur, dumped on the floor beside
    the scanner — foreshadowing dressing for a monster the player has not
    met yet (see pm_demo.c's attract reel and pm_arrival.c for where he
    actually appears). Abstract tapered tubes in this room's own steel/rust
    palette, NOT the real centaur mesh (tools/blender/centaur.py) — pulling
    that generator's exact geometry into a different generator's coordinate
    space bought correctness this scene doesn't need over just building the
    same silhouette with this file's own primitives.

    Sits west of the scanner (more negative X than the machine's own
    R_OUT=92 radius clears), on the floor, out of the walk-up path a player
    or Horner's intake sequence takes to the machine's face.
    """
    # A leg: thigh-thick, tapering to an ankle stump. Each ring's own radius
    # is used as its Y-centre, so the tube's underside sits flush on the
    # floor along its whole taper rather than at one end only.
    LX0, LX1 = -230.0, -140.0
    LR0, LR1 = 26.0, 14.0
    LZ = -90.0
    la, lca = ring("x", LX0, LR0, 8, STEEL, LR0, LZ, group="debris")
    lb, lcb = ring("x", LX1, LR1, 8, STEEL_DARK, LR1, LZ, group="debris")
    loft(la, lca, lb, lcb, "debris")
    lcap_a = vert((LX0 - 4.0, LR0, LZ), RUST)
    disc(la, lca, lcap_a, (LX1 + 100.0, LR0, LZ), "debris")
    lcap_b = vert((LX1 + 4.0, LR1, LZ), RUST)
    disc(lb, lcb, lcap_b, (LX0 - 100.0, LR1, LZ), "debris")

    # A gun-arm: slimmer, dumped at an angle nearby, tapering the other way.
    AX0, AX1 = -150.0, -70.0
    AR0, AR1 = 12.0, 20.0
    AZ = -170.0
    aa, aca = ring("x", AX0, AR0, 8, STEEL_DARK, AR0, AZ, group="debris")
    ab, acb = ring("x", AX1, AR1, 8, STEEL, AR1, AZ, group="debris")
    loft(aa, aca, ab, acb, "debris")
    acap_a = vert((AX0 - 4.0, AR0, AZ), BLACKPLAST)
    disc(aa, aca, acap_a, (AX1 + 100.0, AR0, AZ), "debris")
    acap_b = vert((AX1 + 4.0, AR1, AZ), BLACKPLAST)
    disc(ab, acb, acap_b, (AX0 - 100.0, AR1, AZ), "debris")


def workstations():
    """CRT bank on a folding table plus a rack that never got bolted down."""
    dx0, dx1 = 196.0, 268.0
    box(dx0, dx1, 70.0, 76.0, -70.0, 140.0, PLASTIC_D, "desk")
    for zc in (-58.0, 128.0):                                # folding legs
        box(dx0 + 8.0, dx0 + 14.0, 0.0, 70.0, zc - 3.0, zc + 3.0, CHROME,
            "desk", faces="xXzZ")
        box(dx1 - 14.0, dx1 - 8.0, 0.0, 70.0, zc - 3.0, zc + 3.0, CHROME,
            "desk", faces="xXzZ")

    for zc in (-32.0, 34.0, 100.0):
        box(dx0 + 6.0, dx1 - 6.0, 76.0, 128.0, zc - 26.0, zc + 26.0,
            BLACKPLAST, "crt")
        s0, s1 = zc - 21.0, zc + 21.0
        a = vert((dx0 + 4.0, 120.0, s0), E_SCREEN, (0.02, 0.02), True)
        b = vert((dx0 + 4.0, 120.0, s1), E_SCREEN, (0.98, 0.02), True)
        c = vert((dx0 + 8.0, 84.0, s1), E_SCREEN, (0.98, 0.98), True)
        d = vert((dx0 + 8.0, 84.0, s0), E_SCREEN, (0.02, 0.98), True)
        quad(a, b, c, d, (400.0, 102.0, zc), MAT_SCREEN, "crt")
        box(dx0 + 12.0, dx1 - 20.0, 76.0, 80.0, zc - 20.0, zc + 20.0,
            PLASTIC_D, "crt")

    box(200.0, 268.0, 0.0, 192.0, 150.0, 220.0, STEEL_DARK, "rack")
    for k, y in enumerate((44.0, 74.0, 104.0, 134.0, 164.0)):
        box(196.0, 200.0, y, y + 9.0, 158.0, 212.0,
            E_LED_R if k % 2 else E_LED_A, "rack", emissive=True, faces="x")
    # mains: a daisy chain nobody is proud of
    box(178.0, 190.0, 8.0, 34.0, 236.0, 268.0, PELICAN, "cable")
    cable([(196.0, 20.0, 226.0), (170.0, 3.0, 168.0), (40.0, 3.0, 110.0),
           (-120.0, 3.0, 48.0), (-250.0, 3.0, 62.0)], 4.0, CABLE, "cable")

    # A stool at the middle CRT station (zc=34 above), the one place in the
    # lab that already reads as "a computer" — Horner's intake sequence
    # (pm_intake.c) sits him here before he ever gets to the scanner. Its
    # own group, "stool", so a caller can measure it separately from "desk"
    # if the seat height ever needs to be read back rather than guessed.
    box(STOOL_X - 14.0, STOOL_X + 14.0, 44.0, 50.0, STOOL_Z - 14.0,
        STOOL_Z + 14.0, PLASTIC_D, "stool")
    box(STOOL_X - 5.0, STOOL_X + 5.0, 0.0, 44.0, STOOL_Z - 5.0, STOOL_Z + 5.0,
        CHROME, "stool", faces="xXzZ")


def benches():
    """A folding table and a plank on cinder blocks.  Nothing is furniture."""
    box(-244.0, -96.0, 84.0, 90.0, 166.0, 226.0, PLASTIC_D, "bench")
    for xc in (-232.0, -110.0):
        box(xc - 3.0, xc + 3.0, 0.0, 84.0, 172.0, 178.0, CHROME, "bench",
            faces="xXzZ")
        box(xc - 3.0, xc + 3.0, 0.0, 84.0, 214.0, 220.0, CHROME, "bench",
            faces="xXzZ")
    # plywood extension on stacked blocks
    box(-92.0, -20.0, 80.0, 86.0, 168.0, 226.0, PLY, "bench")
    for xc in (-84.0, -30.0):
        box(xc - 10.0, xc + 10.0, 0.0, 80.0, 190.0, 212.0, CINDER, "bench")

    for tx in (-208.0, -172.0, -136.0):                      # culture tanks
        base, cb = ring("y", 90.0, 15.0, 6, GLASS_DK, tx, 194.0, group="tank")
        top, ct = ring("y", 148.0, 15.0, 6, GLASS_DK, tx, 194.0, group="tank")
        loft(base, cb, top, ct, "tank")
        gl, cg = ring("y", 138.0, 12.5, 6, E_TANK, tx, 194.0, group="tank",
                      emissive=True)
        core = vert((tx, 138.0, 194.0), E_TANK, emissive=True)
        disc(gl, cg, core, (tx, 60.0, 194.0), "tank")
        box(tx - 16.0, tx + 16.0, 148.0, 156.0, 178.0, 210.0, BLACKPLAST,
            "tank", faces="xXYzZ")

    cf, ccf = ring("y", 86.0, 24.0, 8, PLASTIC, -56.0, 196.0, group="lab")
    ct, cct = ring("y", 112.0, 24.0, 8, PLASTIC, -56.0, 196.0, group="lab")
    loft(cf, ccf, ct, cct, "lab")                            # centrifuge
    lid = vert((-56.0, 118.0, 196.0), PLASTIC_D)
    disc(ct, cct, lid, (-56.0, 60.0, 196.0), "lab")

    box(-238.0, -212.0, 90.0, 98.0, 178.0, 208.0, CHROME, "lab")   # scope
    box(-230.0, -220.0, 98.0, 158.0, 194.0, 204.0, CHROME, "lab")
    box(-238.0, -212.0, 140.0, 150.0, 176.0, 200.0, BLACKPLAST, "lab")

    # transit cases doing duty as storage, still stacked where they landed
    box(20.0, 92.0, 0.0, 38.0, 178.0, 226.0, PELICAN, "cases")
    box(24.0, 88.0, 38.0, 72.0, 182.0, 222.0, PELICAN, "cases")
    box(104.0, 168.0, 0.0, 44.0, 186.0, 226.0, CRATE, "cases")


def services():
    """Pipes, clamp lamps on drop cords, tarps, and a lot of gaffer tape."""
    for px, r, col in ((-190.0, 11.0, RUST), (150.0, 13.0, RUST)):
        a, ca = ring("z", -RZ, r, 6, col, px, RY - 26.0, group="pipe")
        b, cb = ring("z", RZ, r, 6, col, px, RY - 26.0, group="pipe")
        loft(a, ca, b, cb, "pipe")
    o, co = ring("x", 150.0, 22.0, 8, RUST, RY - 26.0, -40.0, group="pipe")
    i, ci = ring("x", 150.0, 14.0, 8, RUST, RY - 26.0, -40.0, group="pipe")
    annulus(o, i, (400.0, RY - 26.0, -40.0), "pipe")

    # clamp lamps: drop cord, a cone, a bare bulb.  One of them is dead.
    for cx, cz, live in ((-96.0, -46.0, True), (118.0, 96.0, True),
                         (-40.0, 150.0, False)):
        box(cx - 1.6, cx + 1.6, 214.0, RY, cz - 1.6, cz + 1.6, CABLE, "light",
            faces="xXzZ")
        top, ctp = ring("y", 214.0, 6.0, 6, STEEL_DARK, cx, cz, group="light")
        rim, crm = ring("y", 200.0, 20.0, 6, STEEL_DARK, cx, cz, group="light")
        loft(top, ctp, rim, crm, "light")
        if live:
            bulb, cbl = ring("y", 202.0, 9.0, 6, E_TUBE, cx, cz,
                            group="light", emissive=True)
            core = vert((cx, 200.0, cz), E_TUBE, emissive=True)
            disc(bulb, cbl, core, (cx, 300.0, cz), "light")

    # polythene sheeting taped over a section of the -Z wall
    grid((-268.0, 0.0, -RZ + 3.0), (150.0, 0, 0), (0, 214.0, 0), 2, 2,
         (0.0, 120.0, -700.0), TARP, MAT_SHADE, "tarp")
    box(-270.0, -116.0, 212.0, 218.0, -RZ + 1.0, -RZ + 5.0, GAFFER, "tarp")

    # cable stapled up the wall and along the ceiling to the scanner
    cable([(-250.0, 6.0, 62.0), (-262.0, 90.0, 40.0), (-264.0, 214.0, 20.0),
           (-180.0, 220.0, -30.0), (-70.0, 220.0, -60.0)], 4.0, CABLE, "cable")

    # tape over the floor run at intervals.  Nothing says "wired up in an
    # afternoon" faster than a cable somebody gaffered to the deck.
    for tx, tz in ((170.0, 168.0), (40.0, 110.0), (-120.0, 48.0),
                   (-230.0, 60.0)):
        ref = (tx, -50.0, tz)
        v = [vert((tx + dx, 1.0, tz + dz), GAFFER) for dx, dz in
             ((-16, -9), (16, -9), (16, 9), (-16, 9))]
        quad(v[0], v[1], v[2], v[3], ref, MAT_SHADE, "tape")
    for (bx, by, bz, fc) in ((-RX + 5.0, 118.0, 168.0, "X"),
                             (26.0, 148.0, -60.0, "Z"),
                             (RX - 5.0, 128.0, -170.0, "x")):
        box(bx - 2.0, bx + 2.0, by, by + 13.0, bz - 20.0, bz + 20.0,
            E_LED_A, "placard", emissive=True, faces=fc)

    box(232.0, 264.0, 0.0, 90.0, -218.0, -156.0, CRATE, "drum")
    box(RX - 9.0, RX - 2.0, 138.0, 178.0, -128.0, -74.0, STEEL_DARK,
        "junction")
    box(RX - 12.0, RX - 9.0, 166.0, 172.0, -120.0, -82.0, E_LED_A, "junction",
        emissive=True, faces="x")


def wet():
    """Puddles and the caustic decal the portholes throw on the floor."""
    for x0, x1, z0, z1 in ((-150.0, -40.0, 60.0, 150.0),
                           (70.0, 190.0, -60.0, 30.0),
                           (-230.0, -150.0, -80.0, -10.0)):
        ref = ((x0 + x1) / 2, -50.0, (z0 + z1) / 2)
        v = [vert((x, 0.7, z), (18, 30, 34)) for x, z in
             ((x0, z0), (x1, z0), (x1, z1), (x0, z1))]
        quad(v[0], v[1], v[2], v[3], ref, MAT_SHADE, "puddle")

    # scroll this tile's S/T every frame and the room starts to breathe
    grid((-230.0, 1.4, 40.0), (420.0, 0, 0), (0, 0, 180.0), 3, 2,
         (0.0, -60.0, 0.0), (255, 255, 255), MAT_CAUSTIC, "caustic",
         tile=150.0, emissive=True)


# ==========================================================================
# the corridor
# ==========================================================================
def hallway():
    z0, z1 = DOOR_Z0, DOOR_Z1
    grid((HALL_X, 0.0, z0), (-HALL_X - RX, 0, 0), (0, 0, z1 - z0), 3, 1,
         (0.0, -400.0, 0.0), LIT_WHITE, MAT_FLOOR, "hall_floor", tile=140.0)
    grid((HALL_X, HALL_Y, z0), (-HALL_X - RX, 0, 0), (0, 0, z1 - z0), 3, 1,
         (0.0, 700.0, 0.0), LIT_WHITE, MAT_WALL, "hall_ceil", tile=160.0)
    grid((HALL_X, 0.0, z0), (-HALL_X - RX, 0, 0), (0, HALL_Y, 0), 3, 2,
         (-450.0, 100.0, -400.0), LIT_WHITE, MAT_WALL, "hall_wall", tile=125.0)
    grid((HALL_X, 0.0, z1), (-HALL_X - RX, 0, 0), (0, HALL_Y, 0), 3, 2,
         (-450.0, 100.0, 500.0), LIT_WHITE, MAT_WALL, "hall_wall", tile=125.0)

    # end bulkhead, four panels around the docking collar opening
    ref = (HALL_X - 200.0, COLLAR_Y, COLLAR_Z)
    oz0, oz1 = COLLAR_Z - 52.0, COLLAR_Z + 52.0
    oy0, oy1 = COLLAR_Y - 52.0, COLLAR_Y + 52.0
    grid((HALL_X, 0.0, z0), (0, 0, oz0 - z0), (0, HALL_Y, 0), 1, 2,
         ref, LIT_WHITE, MAT_WALL, "hall_end", tile=125.0)
    grid((HALL_X, 0.0, oz1), (0, 0, z1 - oz1), (0, HALL_Y, 0), 1, 2,
         ref, LIT_WHITE, MAT_WALL, "hall_end", tile=125.0)
    grid((HALL_X, 0.0, oz0), (0, 0, oz1 - oz0), (0, oy0, 0), 1, 1,
         ref, LIT_WHITE, MAT_WALL, "hall_end", tile=125.0)
    grid((HALL_X, oy1, oz0), (0, 0, oz1 - oz0), (0, HALL_Y - oy1, 0), 1, 1,
         ref, LIT_WHITE, MAT_WALL, "hall_end", tile=125.0)

    # two strip lamps zip-tied to the ceiling; the far one is failing
    for cx, live in ((-330.0, True), (-560.0, False)):
        box(cx - 34.0, cx + 34.0, HALL_Y - 9.0, HALL_Y, COLLAR_Z - 9.0,
            COLLAR_Z + 9.0, STEEL_DARK, "hall_light")
        box(cx - 28.0, cx + 28.0, HALL_Y - 11.0, HALL_Y - 9.0, COLLAR_Z - 5.0,
            COLLAR_Z + 5.0, E_TUBE if live else BLACKPLAST, "hall_light",
            emissive=live, faces="y")

    # the loom follows you down the corridor at shoulder height
    cable([(-282.0, 150.0, 18.0), (-380.0, 156.0, 16.0),
           (-500.0, 150.0, 17.0), (-612.0, 140.0, 22.0)], 6.0, CABLE,
          "hall_cable")
    cable([(-282.0, 4.0, 120.0), (-400.0, 3.0, 118.0),
           (-610.0, 3.0, 116.0)], 4.0, CABLE, "hall_cable")
    for cx in (-330.0, -430.0, -530.0):
        box(cx - 8.0, cx + 8.0, 142.0, 160.0, 12.0, 15.0, GAFFER, "hall_cable")

    box(-300.0, -297.0, 150.0, 172.0, 22.0, 64.0, E_LED_A, "hall_sign",
        emissive=True, faces="x")                            # taped-up arrow
    box(-470.0, -469.0, 154.0, 170.0, 14.0, 52.0, E_LED_A, "hall_sign",
        emissive=True, faces="x")

    for x0, x1 in ((-360.0, -300.0), (-540.0, -470.0)):
        ref = ((x0 + x1) / 2, -50.0, COLLAR_Z)
        v = [vert((x, 0.7, z), (18, 30, 34)) for x, z in
             ((x0, 26.0), (x1, 26.0), (x1, 112.0), (x0, 112.0))]
        quad(v[0], v[1], v[2], v[3], ref, MAT_SHADE, "hall_puddle")


# ==========================================================================
# the submersible hatch
# ==========================================================================
def dock():
    """Collar flange, trunk, and the sub's cabin light at the far end."""
    N = 8
    ref_in = (HALL_X - 400.0, COLLAR_Y, COLLAR_Z)

    # flange: an octagon wide enough to cover the square hole's corners
    fo, cfo = ring("x", HALL_X + 1.0, COLLAR_OUT, N, CHROME, COLLAR_Y,
                   COLLAR_Z, group="collar")
    fi, cfi = ring("x", HALL_X + 1.0, COLLAR_R, N, STEEL_DARK, COLLAR_Y,
                   COLLAR_Z, group="collar")
    annulus(fo, fi, ref_in, "collar")
    # hazard band, hand-painted round the lip
    ha, cha = ring("x", HALL_X + 3.0, COLLAR_R + 9.0, N, E_LED_A, COLLAR_Y,
                   COLLAR_Z, group="collar", emissive=True)
    hb, chb = ring("x", HALL_X + 3.0, COLLAR_R + 2.0, N, E_LED_A, COLLAR_Y,
                   COLLAR_Z, group="collar", emissive=True)
    annulus(ha, hb, ref_in, "collar")
    for i in range(8):
        a = 2.0 * math.pi * (i + 0.5) / 8.0
        by = COLLAR_Y + (COLLAR_OUT - 9.0) * math.cos(a)
        bz = COLLAR_Z + (COLLAR_OUT - 9.0) * math.sin(a)
        box(HALL_X - 1.0, HALL_X + 4.0, by - 3.0, by + 3.0, bz - 3.0, bz + 3.0,
            CHROME, "collar", faces="X")

    # trunk: a short tube through the hull to the sub's own hatch
    ta, cta = ring("x", HALL_X, COLLAR_R, N, STEEL_DARK, COLLAR_Y, COLLAR_Z,
                   group="trunk")
    tb, ctb = ring("x", HALL_X - 44.0, COLLAR_R - 3.0, N, BLACKPLAST,
                   COLLAR_Y, COLLAR_Z, group="trunk")
    tc, ctc = ring("x", TRUNK_X + 6.0, COLLAR_R - 6.0, N, STEEL_DARK,
                   COLLAR_Y, COLLAR_Z, group="trunk")
    loft(ta, cta, tb, ctb, "trunk")
    loft(tb, ctb, tc, ctc, "trunk")

    # the sub's hatch ring and the warm cabin behind it
    ha2, cha2 = ring("x", TRUNK_X + 4.0, COLLAR_R - 6.0, N, SUB_HULL,
                     COLLAR_Y, COLLAR_Z, group="sub")
    hb2, chb2 = ring("x", TRUNK_X + 4.0, COLLAR_R - 20.0, N, SUB_HULL,
                     COLLAR_Y, COLLAR_Z, group="sub")
    annulus(ha2, hb2, ref_in, "sub")
    cab, ccab = ring("x", TRUNK_X, COLLAR_R - 20.0, N, E_SUB, COLLAR_Y,
                     COLLAR_Z, group="sub", emissive=True)
    core = vert((TRUNK_X - 4.0, COLLAR_Y, COLLAR_Z), E_SUB, emissive=True)
    disc(cab, ccab, core, ref_in, "sub")
    # a seat back and a control yoke, in silhouette against the cabin light
    box(TRUNK_X - 6.0, TRUNK_X - 2.0, COLLAR_Y - 26.0, COLLAR_Y + 4.0,
        COLLAR_Z - 12.0, COLLAR_Z + 12.0, BLACKPLAST, "sub", faces="X")
    box(TRUNK_X - 3.0, TRUNK_X - 1.0, COLLAR_Y + 6.0, COLLAR_Y + 10.0,
        COLLAR_Z - 20.0, COLLAR_Z + 20.0, BLACKPLAST, "sub", faces="X")
    # umbilical from the sub back up the corridor: he never stowed it
    cable([(HALL_X - 20.0, COLLAR_Y - 30.0, COLLAR_Z),
           (HALL_X + 20.0, 40.0, COLLAR_Z + 20.0),
           (HALL_X + 90.0, 4.0, COLLAR_Z + 34.0),
           (HALL_X + 190.0, 4.0, COLLAR_Z + 12.0)], 5.0, CABLE, "umbilical")


def build():
    set_zone("room")
    shell()
    doorframe()
    porthole(-120.0, 175.0)
    porthole(60.0, 175.0)
    scanner()
    debris()
    workstations()
    benches()
    services()
    wet()
    set_zone("hall")
    hallway()
    set_zone("dock")
    dock()
    bake()


# ==========================================================================
# vertex lighting bake
# ==========================================================================
def bake():
    n = len(POS)
    nrm = [[0.0, 0.0, 0.0] for _ in range(n)]
    for a, b, c, _, _, _ in TRIS:
        fn = _face_normal(a, b, c)
        for v in (a, b, c):
            for k in range(3):
                nrm[v][k] += fn[k]
    for i in range(n):
        if i in EMISSIVE:
            continue
        ln = math.sqrt(sum(k * k for k in nrm[i])) or 1.0
        nv = [k / ln for k in nrm[i]]
        acc = list(AMBIENT)
        px, py, pz = POS[i]
        for lx, ly, lz, lc, rad, inten in LIGHTS:
            dx, dy, dz = lx - px, ly - py, lz - pz
            d = math.sqrt(dx * dx + dy * dy + dz * dz) or 1e-6
            if d >= rad:
                continue
            att = (1.0 - d / rad) ** 2
            ndl = (dx * nv[0] + dy * nv[1] + dz * nv[2]) / d
            ndl = 0.34 + 0.66 * max(0.0, ndl)        # wrap, so nothing is void
            g = att * ndl * inten
            for k in range(3):
                acc[k] += lc[k] / 255.0 * g
        r, g, b = COL[i]
        COL[i] = tuple(min(255, int(round(v * a)))
                       for v, a in ((r, acc[0]), (g, acc[1]), (b, acc[2])))


# ==========================================================================
# textures — 32x32 CI4, one 16-entry TLUT each
# ==========================================================================
def _save_ci4(path, px, pal):
    from PIL import Image
    W = len(px)
    img = Image.new("P", (W, W))
    flat = []
    for c in pal:
        flat.extend(c[:3])
    img.putpalette(flat)
    img.putdata([px[y][x] for y in range(W) for x in range(W)])
    img.save(path)
    ci4 = bytearray()
    for y in range(W):
        for x in range(0, W, 2):
            ci4.append((px[y][x] << 4) | px[y][x + 1])
    tlut = bytearray()
    for c in pal:
        r, g, b = c[0], c[1], c[2]
        a = c[3] if len(c) > 3 else 1
        v = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a & 1)
        tlut += bytes(((v >> 8) & 0xFF, v & 0xFF))
    return len(ci4), len(tlut)


def tex_wall(path):
    W = 32
    pal = [(170, 180, 182), (120, 130, 132), (74, 82, 84), (48, 54, 56),
           (208, 216, 216), (158, 106, 66), (116, 78, 48), (88, 122, 90),
           (58, 86, 62), (140, 150, 152), (40, 46, 48), (218, 226, 228)] \
        + [(0, 0, 0)] * 4
    px = [[0] * W for _ in range(W)]
    for y in range(W):
        for x in range(W):
            c = 0
            if x % 16 in (0, 1):
                c = 2                                  # panel seam
            elif y in (0, 1, 30, 31):
                c = 1
            elif (x % 16) in (3, 13) and y % 8 == 4:
                c = 4                                  # rivet highlight
            elif (x % 16) in (4, 14) and y % 8 == 5:
                c = 3                                  # rivet shadow
            elif (x * 7 + y * 3) % 23 == 0:
                c = 9
            if x % 16 in (2, 3) and y > 6:
                c = 5 if (y + x) % 3 else 6            # rust weeping down
            if x % 16 in (14, 15) and y > 14:
                c = 7 if (y * 2 + x) % 4 else 8        # algae
            px[y][x] = c
    return _save_ci4(path, px, pal)


def tex_floor(path):
    W = 32
    pal = [(134, 144, 146), (88, 96, 98), (44, 50, 52), (26, 30, 32),
           (152, 164, 166), (74, 112, 106), (46, 76, 78), (112, 122, 124),
           (88, 118, 90), (20, 24, 26), (176, 190, 192), (58, 66, 68)] \
        + [(0, 0, 0)] * 4
    px = [[0] * W for _ in range(W)]
    for y in range(W):
        for x in range(W):
            gx, gy = x % 16, y % 16
            if gx < 2 or gy < 2:
                c = 4                                  # bar top
            elif 2 <= gx < 4 or 2 <= gy < 4:
                c = 1                                  # bar side
            else:
                c = 3 if (gx + gy) % 5 else 2          # void below the grate
            if (x * 5 + y * 11) % 29 == 0:
                c = 10                                 # standing-water sheen
            if 8 < x < 15 and 18 < y < 27:
                c = 5 if (x + y) % 2 else 6            # algae mat
            px[y][x] = c
    return _save_ci4(path, px, pal)


def tex_screen(path):
    W = 32
    pal = [(10, 34, 24), (16, 54, 38), E_SCREEN, (60, 190, 124), (34, 116, 78),
           (190, 255, 220), (8, 22, 16), (44, 150, 100)] + [(0, 0, 0)] * 8
    px = [[0] * W for _ in range(W)]
    for y in range(W):
        for x in range(W):
            c = 0 if y % 2 else 1                      # scanlines
            if y < 4 and 2 < x < 30:
                c = 3 if (x + y) % 3 else 4            # header text block
            if 6 <= y <= 17:
                wave = 12 + int(4.6 * math.sin(x * 0.62) +
                                2.4 * math.sin(x * 1.9 + 1.1))
                if abs(y - wave) <= 1:
                    c = 5 if y % 2 == 0 else 2         # trace
                elif y > wave:
                    c = 7 if (x + y) % 4 == 0 else c
            if y >= 20 and (x // 3 + y) % 7 < 2 and x < 28:
                c = 3                                  # log lines
            if x in (0, 31) or y in (0, 31):
                c = 6
            px[y][x] = c
    return _save_ci4(path, px, pal)


def tex_caustic(path):
    """1-bit alpha decal: index 0 is fully transparent."""
    W = 32
    pal = [(0, 0, 0, 0), (40, 96, 104, 1), (74, 152, 158, 1),
           (120, 206, 206, 1), (176, 240, 232, 1)] + [(0, 0, 0, 0)] * 11
    px = [[0] * W for _ in range(W)]
    for y in range(W):
        for x in range(W):
            u, v = x / W * 2 * math.pi, y / W * 2 * math.pi
            n = (math.sin(u * 2 + math.sin(v)) +
                 math.sin(v * 2 + math.cos(u * 1.5)) +
                 0.7 * math.sin((u + v) * 3))
            a = abs(n)
            px[y][x] = 4 if a > 2.15 else 3 if a > 1.75 else \
                2 if a > 1.35 else 1 if a > 1.05 else 0
    return _save_ci4(path, px, pal)


# ==========================================================================
# exporters
# ==========================================================================
def write_obj(path, mtl):
    with open(path, "w") as f:
        f.write("# Horner wet lab - N64 budget interior\n")
        f.write("# %d verts / %d tris\n" % (len(POS), len(TRIS)))
        f.write("mtllib %s\no dank_lab\n" % mtl)
        for p, c in zip(POS, COL):
            f.write("v %.2f %.2f %.2f %.4f %.4f %.4f\n"
                    % (p[0], p[1], p[2], c[0] / 255, c[1] / 255, c[2] / 255))
        for u, v in UVS:
            f.write("vt %.5f %.5f\n" % (u, 1.0 - v))
        cur = None
        for a, b, c, mat, _, _ in sorted(TRIS, key=lambda t: t[3]):
            if mat != cur:
                f.write("usemtl %s\n" % mat)
                cur = mat
            f.write("f %d/%d %d/%d %d/%d\n"
                    % (a + 1, a + 1, b + 1, b + 1, c + 1, c + 1))


def write_mtl(path):
    maps = {"shade": None, "wall": "lab_wall.png", "floor": "lab_floor.png",
            "screen": "lab_screen.png", "caustic": "lab_caustic.png"}
    with open(path, "w") as f:
        for name, tex in maps.items():
            f.write("newmtl %s\nKd 1 1 1\nillum 1\n" % name)
            if tex:
                f.write("map_Kd %s\n" % tex)
            f.write("\n")


def batch(tris, cache=32):
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


DL_SETUP = {
    MAT_SHADE: ["    gsDPPipeSync(),",
                "    gsSPTexture(0, 0, 0, 0, G_OFF),",
                "    gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE),",
                "    gsDPSetRenderMode(G_RM_FOG_SHADE_A, G_RM_AA_ZB_OPA_SURF2),"],
    MAT_WALL: ["    gsDPPipeSync(),",
               "    /* load lab_wall: 32x32 CI4 + TLUT, G_TX_WRAP both axes */",
               "    gsSPTexture(0x8000, 0x8000, 0, G_TX_RENDERTILE, G_ON),",
               "    gsDPSetCombineMode(G_CC_MODULATEIDECALA, G_CC_MODULATEIDECALA),",
               "    gsDPSetRenderMode(G_RM_FOG_SHADE_A, G_RM_AA_ZB_OPA_SURF2),"],
    MAT_FLOOR: ["    gsDPPipeSync(),",
                "    /* load lab_floor: 32x32 CI4 + TLUT, G_TX_WRAP both axes */",
                "    gsSPTexture(0x8000, 0x8000, 0, G_TX_RENDERTILE, G_ON),",
                "    gsDPSetCombineMode(G_CC_MODULATEIDECALA, G_CC_MODULATEIDECALA),",
                "    gsDPSetRenderMode(G_RM_FOG_SHADE_A, G_RM_AA_ZB_OPA_SURF2),"],
    MAT_SCREEN: ["    gsDPPipeSync(),",
                 "    /* load lab_screen: 32x32 CI4 + TLUT, G_TX_CLAMP both axes */",
                 "    gsSPTexture(0x8000, 0x8000, 0, G_TX_RENDERTILE, G_ON),",
                 "    /* TEXEL0 only - screens ignore fog and vertex light */",
                 "    gsDPSetCombineMode(G_CC_DECALRGB, G_CC_DECALRGB),",
                 "    gsDPSetRenderMode(G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2),"],
    MAT_CAUSTIC: ["    gsDPPipeSync(),",
                  "    /* lab_caustic: index 0 has TLUT alpha 0 - 1-bit cutout.",
                  "     * Advance the tile origin every frame to make it crawl:",
                  "     *   gDPSetTileSize(gfx++, G_TX_RENDERTILE,",
                  "     *                  s_ofs & 0x3FF, t_ofs & 0x3FF, ...); */",
                  "    gsSPTexture(0x8000, 0x8000, 0, G_TX_RENDERTILE, G_ON),",
                  "    gsDPSetCombineMode(G_CC_MODULATEIA, G_CC_MODULATEIA),",
                  "    gsDPSetRenderMode(G_RM_ZB_XLU_DECAL, G_RM_ZB_XLU_DECAL2),"],
}

MAT_ORDER = [MAT_SHADE, MAT_WALL, MAT_FLOOR, MAT_SCREEN, MAT_CAUSTIC]


def write_header(path, scale, texinfo):
    buckets = {}
    for a, b, c, mat, _, zone in TRIS:
        buckets.setdefault((zone, mat), []).append((a, b, c))

    def s16(v):
        return max(-32768, min(32767, int(round(v * scale))))

    def tc(u):
        return max(-32768, min(32767, int(round(u * TEX_W * 32))))

    L = []
    w = L.append
    zc = {z: sum(1 for t in TRIS if t[5] == z) for z in ZONES}
    w("/* dank_lab.h - generated by dank_lab_gen.py, do not edit")
    w(" *")
    w(" * Horner wet lab | %d verts / %d tris total" % (len(POS), len(TRIS)))
    w(" *   room  %4d tris   %.0f x %.0f x %.0f cm"
      % (zc["room"], 2 * RX, 2 * RZ, RY))
    w(" *   hall  %4d tris   corridor to the sub" % zc["hall"])
    w(" *   dock  %4d tris   collar, trunk, cabin light" % zc["dock"])
    w(" *")
    w(" * F3DEX2, 32-vertex cache, s16 model space at %d units/cm." % scale)
    w(" * Lighting is BAKED into vertex colour - leave G_LIGHTING off.")
    w(" * Interior faces wind inward; keep G_CULL_BACK on.")
    w(" *")
    w(" * ZONES: draw the one you are standing in, plus whatever the doorway")
    w(" * portal can see.  lab_dl_all exists for debugging only - chaining it")
    w(" * puts every triangle in the level through the RSP at once.")
    w(" */")
    w("#ifndef DANK_LAB_H")
    w("#define DANK_LAB_H")
    w("")
    w("#define LAB_MODEL_SCALE   %d" % scale)
    w("#define LAB_HALF_X        %d   /* s16 units */" % s16(RX))
    w("#define LAB_HALF_Z        %d" % s16(RZ))
    w("#define LAB_CEILING       %d" % s16(RY))
    w("#define LAB_VERTEX_COUNT  %d" % len(POS))
    w("#define LAB_TRI_COUNT     %d" % len(TRIS))
    w("")
    w("/* Doorway portal in the -X wall - test this rect to decide whether")
    w(" * the corridor zone needs drawing from inside the room. */")
    w("#define LAB_DOOR_X        %d" % s16(-RX))
    w("#define LAB_DOOR_Z0       %d" % s16(DOOR_Z0))
    w("#define LAB_DOOR_Z1       %d" % s16(DOOR_Z1))
    w("#define LAB_DOOR_TOP      %d" % s16(DOOR_Y))
    w("#define LAB_HALL_END      %d   /* docking collar face */" % s16(HALL_X))
    w("")
    w("/* Underwater murk.  Start the fog close - the far wall should be")
    w(" * half-eaten at 4.6 m or the room stops feeling flooded. */")
    w("#define LAB_FOG_R          8")
    w("#define LAB_FOG_G         20")
    w("#define LAB_FOG_B         24")
    w("#define LAB_FOG_MIN       900    /* gSPFogPosition(min, max); ~3.8 m */")
    w("#define LAB_FOG_MAX       1000   /* saturated by ~15 m */")
    w("")
    w("static const Gfx lab_dl_fog[] = {")
    w("    gsDPSetFogColor(LAB_FOG_R, LAB_FOG_G, LAB_FOG_B, 255),")
    w("    gsSPFogPosition(LAB_FOG_MIN, LAB_FOG_MAX),")
    w("    gsSPSetGeometryMode(G_SHADE | G_SHADING_SMOOTH | G_CULL_BACK"
      " | G_ZBUFFER | G_FOG),")
    w("    gsSPClearGeometryMode(G_LIGHTING | G_TEXTURE_GEN),")
    w("    gsSPEndDisplayList(),")
    w("};")
    w("")

    for zone in ZONES:
        for mat in MAT_ORDER:
            key = (zone, mat)
            if key not in buckets:
                continue
            loads = batch(buckets[key])
            vname = "lab_vtx_%s_%s" % (zone, mat)
            flat = [v for vl, _ in loads for v in vl]
            w("static const Vtx %s[] = {   /* %d tris */"
              % (vname, len(buckets[key])))
            for v in flat:
                x, y, z = POS[v]
                u, t = UVS[v]
                r, g, b = COL[v]
                w("    {{{%6d,%6d,%6d}, 0, {%6d,%6d}, {%3d,%3d,%3d,255}}},"
                  % (s16(x), s16(y), s16(z), tc(u), tc(t), r, g, b))
            w("};")
            w("")
            w("static const Gfx lab_dl_%s_%s[] = {" % (zone, mat))
            for line in DL_SETUP[mat]:
                w(line)
            base = 0
            for vl, tl in loads:
                w("    gsSPVertex(&%s[%d], %d, 0)," % (vname, base, len(vl)))
                i = 0
                while i + 1 < len(tl):
                    a, b = tl[i], tl[i + 1]
                    w("    gsSP2Triangles(%d,%d,%d, 0, %d,%d,%d, 0),"
                      % (a[0], a[1], a[2], b[0], b[1], b[2]))
                    i += 2
                if i < len(tl):
                    w("    gsSP1Triangle(%d,%d,%d, 0)," % tl[i])
                base += len(vl)
            w("    gsSPEndDisplayList(),")
            w("};")
            w("")

        w("/* zone: %s (%d tris).  Opaque first, caustic decal last. */"
          % (zone, zc[zone]))
        w("static const Gfx lab_dl_%s[] = {" % zone)
        for mat in MAT_ORDER:
            if (zone, mat) in buckets:
                w("    gsSPDisplayList(lab_dl_%s_%s)," % (zone, mat))
        w("    gsSPEndDisplayList(),")
        w("};")
        w("")

    w("/* Debug only.  See the ZONES note at the top of this file. */")
    w("static const Gfx lab_dl_all[] = {")
    w("    gsSPDisplayList(lab_dl_fog),")
    for zone in ZONES:
        w("    gsSPDisplayList(lab_dl_%s)," % zone)
    w("    gsSPEndDisplayList(),")
    w("};")
    w("")
    w("/* texture data budget (TMEM texel area is 2 KB with TLUTs resident) */")
    for name, (nb, nt) in texinfo.items():
        w("/*   %-12s %4d B texels + %2d B TLUT */" % (name, nb, nt))
    w("")
    w("#endif /* DANK_LAB_H */")
    open(path, "w").write("\n".join(L) + "\n")


# ==========================================================================
# --emit-header: publish this room's dimensions to the runtime
#
# The same discipline tools/blender/pm_world.py's emit_header() established for
# the island, applied to the lab. The rule it enforces: never type a dimension
# into C that geometry already defines.
#
# Before this, PetaByte-Madness/src/pm_lab.h carried
#
#     #define PM_LAB_REAL_X0   (-452.0f)
#     #define PM_LAB_DESK_X    (113.92f)  // workstations(): STOOL_X (178) * 0.64
#
# — six room extents and three fixture positions, each derived BY HAND from
# this file's constants with the multiply written out in a comment. Three
# separate consumers (pm_lab.c's collision, pm_demo.c's LAB_CINE camera,
# pm_intake.c's intake camera) then keyed off those literals, and pm_intake.c
# additionally restated the scanner's centre as `POS_MRI_X (-70.0f * CM)`.
#
# That arrangement has already failed once in this project in the same shape:
# pm_models.h's PM_ISLAND_HALF_W was a hand-derived literal that silently
# drifted from the generator's real radius when the island was regenerated,
# and every camera keyed off it was then wrong. There is nothing structurally
# different about these numbers.
#
# ── Units ─────────────────────────────────────────────────────────────────
# This generator authors in CENTIMETRES. The runtime is 64 world units to the
# metre (pm_lab.h), so world = cm * 0.01 m/cm * 64 u/m = cm * 0.64. Every value
# emitted below is already multiplied, so no call site ever performs that
# conversion — which is the whole point, since `* CM` scattered through
# pm_intake.c is how a wrong factor hides.
# ==========================================================================
CM_TO_WORLD = 0.64


def measure():
    """Measure the BUILT mesh, not the constants. build() must have run."""
    xs = [p[0] for p in POS]
    ys = [p[1] for p in POS]
    zs = [p[2] for p in POS]
    return {
        "x0": min(xs), "x1": max(xs),
        "y0": min(ys), "y1": max(ys),
        "z0": min(zs), "z1": max(zs),
    }


def emit_header(path):
    m = measure()
    w = lambda cm: cm * CM_TO_WORLD   # noqa: E731
    L = []
    add = L.append
    add("// SPDX-License-Identifier: MPL-2.0")
    add("//")
    add("// pm_lab_gen.h — GENERATED by PetaByte-Madness/tools/dank_lab_gen.py.")
    add("// Do not edit.")
    add("//")
    add("// Regenerate:")
    add("//   python3 PetaByte-Madness/tools/dank_lab_gen.py \\")
    add("//       --emit-header PetaByte-Madness/src/pm_lab_gen.h")
    add("//")
    add("// Every value is in WORLD units: this generator authors in")
    add("// centimetres and the runtime is 64 units to the metre, so the")
    add("// factor %.2f is already applied here and NO call site should" % CM_TO_WORLD)
    add("// apply it again. See the generator's emit_header comment for the")
    add("// hand-derived literals this replaces and why they were a hazard.")
    add("")
    add("#ifndef PM_LAB_GEN_H")
    add("#define PM_LAB_GEN_H")
    add("")
    add("// ── The room, MEASURED from the built mesh ──────────────────────────")
    add("// Not from the envelope constants: the shell is built from RX/RZ/RY but")
    add("// the hall and the dock run well outside it, and the collision has to")
    add("// match what is actually drawn.")
    add("#define PM_LAB_X0   (%.1ff)" % w(m["x0"]))
    add("#define PM_LAB_X1   (%.1ff)" % w(m["x1"]))
    add("#define PM_LAB_Y0   (%.1ff)" % w(m["y0"]))
    add("#define PM_LAB_Y1   (%.1ff)" % w(m["y1"]))
    add("#define PM_LAB_Z0   (%.1ff)" % w(m["z0"]))
    add("#define PM_LAB_Z1   (%.1ff)" % w(m["z1"]))
    add("")
    add("// The main room's own envelope, without the hall and dock. This is the")
    add("// volume a camera framing \"the lab\" should stay inside.")
    add("#define PM_LAB_ROOM_HALF_X  (%.1ff)" % w(RX))
    add("#define PM_LAB_ROOM_HALF_Z  (%.1ff)" % w(RZ))
    add("#define PM_LAB_CEILING      (%.1ff)" % w(RY))
    add("")
    add("// ── The doorway out, and what is beyond it ──────────────────────────")
    add("#define PM_LAB_DOOR_Z0   (%.1ff)" % w(DOOR_Z0))
    add("#define PM_LAB_DOOR_Z1   (%.1ff)" % w(DOOR_Z1))
    add("#define PM_LAB_DOOR_Y    (%.1ff)" % w(DOOR_Y))
    add("#define PM_LAB_HALL_X    (%.1ff)  // far end of the corridor" % w(HALL_X))
    add("#define PM_LAB_TRUNK_X   (%.1ff)  // submersible hatch face" % w(TRUNK_X))
    add("#define PM_LAB_COLLAR_Y  (%.1ff)" % w(COLLAR_Y))
    add("#define PM_LAB_COLLAR_Z  (%.1ff)" % w(COLLAR_Z))
    add("#define PM_LAB_COLLAR_R  (%.1ff)" % w(COLLAR_R))
    add("")
    add("// ── The MRI scanner ────────────────────────────────────────────────")
    add("// The fixture the whole intro is about. pm_intake.c used to restate")
    add("// its centre as POS_MRI_X (-70.0f * CM).")
    add("#define PM_MRI_X       (%.1ff)" % w(SCANNER_CX))
    add("#define PM_MRI_Y       (%.1ff)  // bore axis height" % w(SCANNER_CY))
    add("#define PM_MRI_Z_FACE  (%.1ff)  // the end he walks up to" % w(SCANNER_ZFACE))
    add("#define PM_MRI_Z_BACK  (%.1ff)" % w(SCANNER_ZBACK))
    add("#define PM_MRI_R_OUT   (%.1ff)  // shell radius; clearance to keep" % w(SCANNER_ROUT))
    add("#define PM_MRI_R_BORE  (%.1ff)  // the hole he goes into" % w(SCANNER_RBORE))
    add("// Centre of the bore mouth: where a camera should look, and where a")
    add("// walk-up path should end.")
    add("#define PM_MRI_MOUTH_Z (%.1ff)" % w(SCANNER_ZFACE))
    add("")
    add("// ── The desk ───────────────────────────────────────────────────────")
    add("#define PM_DESK_X   (%.1ff)  // workstations(): STOOL_X" % w(STOOL_X))
    add("#define PM_DESK_Z   (%.1ff)  // workstations(): STOOL_Z" % w(STOOL_Z))
    add("#define PM_DESK_SEAT_Y (%.1ff)  // the stool's top face" % w(50.0))
    add("")
    add("// ── The authored light rig ─────────────────────────────────────────")
    add("// This generator bakes vertex lighting from a %d-fixture rig and then")
    add("// throws the rig away — the OBJ carries the result, not the sources.")
    add("// The runtime lit this room with ONE directional light plus")
    add("// kiln_engine.c's generic \"avoid pure black\" ambient of 45, which is a")
    add("// bootstrap default and not an exposure anyone chose for a flooded")
    add("// station. Publishing the rig lets pm_env.c derive its key and fill")
    add("// from the light the room was actually authored under.")
    add("//")
    add("// Positions are world units; colour is 0-255; radius is world units;")
    add("// intensity is the generator's own 0..~1 scalar. Sorted brightest")
    add("// first (intensity / distance falls off, so the strongest fixtures")
    add("// come first) because KilnScene carries only")
    add("// KILN_SCENE_MAX_LIGHTS of them and the consumer takes a prefix.")
    add("#define PM_LAB_LIGHT_COUNT %d" % len(LIGHTS))
    add("")
    add("// { x, y, z, r, g, b, radius, intensity }")
    add("#define PM_LAB_LIGHT_TABLE \\")
    ranked = sorted(LIGHTS, key=lambda l: -l[5])
    for i, (lx, ly, lz, col, rad, inten) in enumerate(ranked):
        cont = " \\" if i + 1 < len(ranked) else ""
        add("    { %8.1ff, %7.1ff, %8.1ff, %3d, %3d, %3d, %7.1ff, %.2ff },%s"
            % (w(lx), w(ly), w(lz), col[0], col[1], col[2], w(rad), inten, cont))
    add("")
    add("// The generator's own ambient floor: \"dank teal, never fully black\".")
    add("// Emitted as 0-255 to match KilnScene.ambient.")
    add("#define PM_LAB_AMBIENT_R %d" % int(round(AMBIENT[0] * 255.0)))
    add("#define PM_LAB_AMBIENT_G %d" % int(round(AMBIENT[1] * 255.0)))
    add("#define PM_LAB_AMBIENT_B %d" % int(round(AMBIENT[2] * 255.0)))
    add("")
    add("#endif // PM_LAB_GEN_H")

    text = "\n".join(L) + "\n"
    # The %d in the light-rig comment above is filled here rather than inline
    # so the count and the table can never disagree.
    text = text.replace("a %d-fixture rig", "a %d-fixture rig" % len(LIGHTS))
    with open(path, "w") as fh:
        fh.write(text)
    print("wrote %s (%d lines)" % (path, len(L)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scale", type=int, default=8)
    ap.add_argument("--outdir", default=".")
    ap.add_argument("--emit-header", metavar="PATH",
                    help="write pm_lab_gen.h and exit (no OBJ, no textures)")
    a = ap.parse_args()

    if a.emit_header:
        # build() only, then measure. No texture generation and no OBJ write:
        # regenerating the header must be cheap enough to do on every edit, and
        # must not touch the committed OBJ as a side effect.
        build()
        emit_header(a.emit_header)
        return
    os.makedirs(a.outdir, exist_ok=True)
    p = lambda n: os.path.join(a.outdir, n)

    build()
    texinfo = {
        "lab_wall": tex_wall(p("lab_wall.png")),
        "lab_floor": tex_floor(p("lab_floor.png")),
        "lab_screen": tex_screen(p("lab_screen.png")),
        "lab_caustic": tex_caustic(p("lab_caustic.png")),
    }
    write_obj(p("dank_lab.obj"), "dank_lab.mtl")
    write_mtl(p("dank_lab.mtl"))
    write_header(p("dank_lab.h"), a.scale, texinfo)

    zc = {}
    for t in TRIS:
        zc[t[5]] = zc.get(t[5], 0) + 1
    print("verts %d   tris %d   emissive verts %d"
          % (len(POS), len(TRIS), len(EMISSIVE)))
    for z in ZONES:
        print("   zone %-5s %4d tris" % (z, zc.get(z, 0)))


if __name__ == "__main__":
    main()
