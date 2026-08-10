#!/usr/bin/env python3
"""
bestiary.py - THE VEIL BESTIARY.

Classic demon anatomy at N64 budgets: horns, hooves, bat wings, barbed tails,
fangs. Every creature is a PHANTOM - its veil-off palette is fully transparent,
so with the veil down it is not drawn at all. Only the eyes survive.
"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gltfkit import Doc, Mesh, box, blade, loft, ring, euler
import parts as P
import anim as A
import textures as TX

OUT = os.environ.get("VEIL_OUT", "/mnt/user-data/outputs/veil")
os.makedirs(OUT, exist_ok=True)
TAU = math.pi * 2
VCOL_FLOOR, VCOL_GAIN = 0.20, 0.72


def weld(m, q=4):
    """Weld on (pos, uv, colour) - the N64 vertex is pos/uv/colour, so this is
    the count the hardware sees. Also lifts enemy vertex colours off the floor:
    enemies get their own gSPSetLights, and if the vertex colour crushes to
    zero the TLUT's bright entries can never reach the screen."""
    m.c = [(VCOL_FLOOR + c[0]*VCOL_GAIN, VCOL_FLOOR + c[1]*VCOL_GAIN,
            VCOL_FLOOR + c[2]*VCOL_GAIN, c[3]) for c in m.c]
    m.smooth_normals()
    key, v, n, uv, c, remap = {}, [], [], [], [], []
    for i in range(len(m.v)):
        k = (tuple(round(x, q) for x in m.v[i]), tuple(round(x, 3) for x in m.uv[i]),
             tuple(round(x, 3) for x in m.c[i]))
        if k not in key:
            key[k] = len(v)
            v.append(m.v[i]); n.append(m.n[i]); uv.append(m.uv[i]); c.append(m.c[i])
        remap.append(key[k])
    m.v, m.n, m.uv, m.c = v, n, uv, c
    m.idx = [remap[i] for i in m.idx]
    return m


def mv(mesh, dx=0.0, dy=0.0, dz=0.0):
    return mesh.transform(lambda p: (p[0]+dx, p[1]+dy, p[2]+dz))


# NOTE: (x,y,z) -> (x,z,y) is a REFLECTION, not a rotation - it flips
# handedness and inverts every normal. These two are real rotations about X.
def lay_back(mesh):
    """Mesh built along -Y now points along -Z. Rotation about X by +90."""
    return mesh.transform(lambda p: (p[0], -p[2], p[1]))


def lay_fwd(mesh):
    """Mesh built along -Y now points along +Z. Rotation about X by -90."""
    return mesh.transform(lambda p: (p[0], p[2], -p[1]))


# ===================================================================== TEXTURES
def make_texture(seed, w, h, style, base_pal):
    if style == "hide":
        fn = lambda u, v, x, y: (TX.fbm(u*3.0, v*3.0, seed)*7
                                 + TX.fbm(u*9.0, v*8.0, seed+3)*5
                                 + 2.0*abs(math.sin(u*11+v*5)) + 1.0)
    elif style == "pelt":
        fn = lambda u, v, x, y: (TX.fbm(u*2.2, v*4.0, seed)*8
                                 + TX.fbm(u*10.0, v*3.0, seed+5)*4
                                 + (2.0 if (y % 5 == 0) else 0) + 0.5)
    elif style == "stone":
        fn = lambda u, v, x, y: (TX.fbm(u*2.6, v*2.6, seed)*6
                                 + TX.fbm(u*7.0, v*7.0, seed+9)*5
                                 + (4.0 if abs(math.sin(u*13+v*9)) > 0.93 else 0) + 1.0)
    else:                                                    # ember / slag
        fn = lambda u, v, x, y: (TX.fbm(u*2.0, v*2.4, seed)*5
                                 + TX.fbm(u*8.0, v*9.0, seed+11)*6
                                 + 5.0*max(0.0, math.sin(v*7+u*3))**3 + 1.0)
    idx = TX.idx_map(w, h, fn)
    idx = [[max(0, min(15, int(round((val-7.2)*1.26 + 8.4)))) for val in row]
           for row in idx]
    return idx, TX.phantom_cold(base_pal), TX.veil_project(base_pal, TX.D_BAND), w*h//2


def add_eyes(d, parent, sep, r, y, z):
    """Every demon carries an eye node. It is the only thing the veil leaves."""
    mat = d.material("eye_tell", rgba=(1.0, 0.16, 0.10, 1.0),
                     emissive=(1.0, 0.22, 0.12))
    j = d.joint("eyes", parent, t=(0, y, z))
    j.mesh, j.mat = weld(P.eye_pair(sep=sep, r=r)), mat
    return j


def make_tail(d, parent, n, ln, r0, dk, md, first_off, barb_len, barb_w, bone,
              taper=0.74, droop=-0.55):
    base = d.joint("tail_base", parent, t=first_off, r=euler(droop, 0, 0))
    first_off = (0, 0, 0)
    tail, prev = [], base
    for i in range(n):
        t = d.joint(f"tail_{i}", prev, t=first_off if i == 0 else (0, 0, -ln))
        t.mesh = weld(lay_back(P.tail_seg(ln, r0, r0*taper, 4, dk, md)))
        tail.append(t)
        prev, r0, ln = t, r0*taper, ln*0.90
    b = d.joint("barb", tail[-1], t=(0, 0, -ln), r=euler(1.57, 0, 0))
    b.mesh = weld(P.barb(barb_len, barb_w, bone, dk))
    return tail, b


# ========================================================================= IMP
def build_imp():
    """Common enemy. Pot-bellied, long-armed, goat-legged, whip tail with a
    barb, curled horns, big pointed ears. Scampers, rears up to swipe."""
    d = Doc("IMP")
    idx, cold, veil, tb = make_texture(31, 32, 32, "hide",
        TX.ramp((0.06, 0.02, 0.02), (0.98, 0.52, 0.36), gamma=0.95))
    mat = d.material("imp_hide", png=TX.png_bytes(TX.render(idx, veil, 1)),
                     emissive=(0.40, 0.04, 0.03))
    dk, md = (0.16, 0.06, 0.05, 1), (0.58, 0.22, 0.18, 1)
    bone = (0.94, 0.90, 0.80, 1)

    root = d.joint("IMP")
    hip = d.joint("hip", root, t=(0, 0.62, 0))
    hip.mesh = weld(loft([ring(-0.10, 0.15, 5), ring(0.06, 0.19, 5)], [dk, md], 5))
    torso = d.joint("torso", hip, t=(0, 0.10, 0))
    # pot belly: widest low, narrow at the chest. Reads instantly as an imp.
    torso.mesh = weld(loft([ring(-0.02, 0.19, 6), ring(0.11, 0.24, 6, sz=0.86),
                            ring(0.26, 0.20, 6, sz=0.80), ring(0.38, 0.15, 6)],
                           [md, md, dk, dk], 6))
    neck = d.joint("neck", torso, t=(0, 0.38, 0.01))
    head = d.joint("head", neck, t=(0, 0.08, 0.02))
    hm = loft([ring(-0.09, 0.08, 5), ring(0.0, 0.125, 5, sz=0.92),
               ring(0.09, 0.06, 5)], [dk, md, dk], 5)
    hm.merge(mv(loft([ring(0, 0.075, 4), ring(-0.02, 0.055, 4)], [md, md], 4), 0, 0.0, 0.10))
    hm.merge(mv(P.fangs(4, 0.11, 0.05), 0, -0.035, 0.14))
    head.mesh = weld(hm)
    jw = d.joint("jaw", head, t=(0, -0.055, 0.06))
    jw.mesh = weld(P.jaw(0.115, 0.11, 0.07, 3, md, bone))

    for s, sx in (("l", -1), ("r", 1)):
        h = d.joint(f"horn_{s}", head, t=(0.055*sx, 0.075, -0.01),
                    r=euler(-0.35, 0, -0.42*sx))
        h.mesh = weld(P.horn(0.20, 0.038, 0.005, 0.75, 0.8*sx, 4, 3, dk, bone))
        e = d.joint(f"ear_{s}", head, t=(0.10*sx, 0.03, -0.02), r=euler(0, 0, 1.35*sx))
        e.mesh = weld(P.ear(0.16, 0.085, md, dk))

    arms = []
    for s, sx in (("l", -1), ("r", 1)):
        up = d.joint(f"upperarm_{s}", torso, t=(0.20*sx, 0.30, 0),
                     r=euler(0.15, 0, 0.62*sx))
        up.mesh = weld(loft([ring(0, 0.072, 4), ring(-0.26, 0.055, 4)], [md, dk], 4))
        lo = d.joint(f"forearm_{s}", up, t=(0, -0.26, 0), r=euler(-0.35, 0, 0))
        lo.mesh = weld(loft([ring(0, 0.055, 4), ring(-0.24, 0.042, 4)], [dk, md], 4))
        hd = d.joint(f"hand_{s}", lo, t=(0, -0.24, 0))
        hd.mesh = weld(P.claw_hand(3, 0.115, 0.40, md, bone))
        arms.append((up, lo, hd))

    legs = []
    for s, sx in (("l", -1), ("r", 1)):
        tm, sm, cm, hm2 = P.digit_leg(0.24, 0.22, 0.15, 0.088, dk, md)
        th = d.joint(f"thigh_{s}", hip, t=(0.105*sx, -0.06, 0.01)); th.mesh = weld(tm)
        sh = d.joint(f"shin_{s}", th, t=(0, -0.24, 0), r=euler(-0.55, 0, 0)); sh.mesh = weld(sm)
        cn = d.joint(f"cannon_{s}", sh, t=(0, -0.22, 0), r=euler(0.75, 0, 0)); cn.mesh = weld(cm)
        hf = d.joint(f"hoof_{s}", cn, t=(0, -0.17, 0), r=euler(-0.25, 0, 0)); hf.mesh = weld(hm2)
        legs.append((th, sh, cn, hf))

    tail, barb = make_tail(d, hip, 4, 0.15, 0.045, dk, md, (0, -0.02, -0.13),
                           0.15, 0.10, bone)
    eyes = add_eyes(d, head, 0.048, 0.016, 0.012, 0.115)
    for j in d.joints:
        if j.mesh and j.mat is None:
            j.mat = mat

    (upl, lol, hdl), (upr, lor, hdr) = arms
    (thl, shl, cnl, hfl), (thr, shr, cnr, hfr) = legs
    TAILC = tail + [barb]

    idle = {
        hip:   {"translation": A.osc_pos(2.6, 0.022, 1, base=(0, 0.62, 0), harm2=0.35)},
        torso: {"scale": A.breathe(torso, 2.6, 0.045),
                "rotation": A.osc_rot(2.6, 0.05, 0, base=(0.14, 0, 0))},
        neck:  {"rotation": A.osc_rot(2.6, 0.07, 1, phase=1.1, base=(-0.12, 0, 0))},
        head:  {"rotation": A.osc_rot(2.6, 0.05, 0, phase=2.2, base=(0.08, 0, 0))},
        jw:    {"rotation": A.rkeys([(0.0, (0, 0, 0)), (1.5, (0.10, 0, 0)),
                                     (1.9, (0.42, 0, 0)), (2.6, (0, 0, 0))], 3)},
        upl:   {"rotation": A.osc_rot(2.6, 0.06, 0, phase=0.6, base=(0.55, 0, -0.62))},
        upr:   {"rotation": A.osc_rot(2.6, 0.06, 0, phase=1.3, base=(0.55, 0, 0.62))},
        lol:   {"rotation": A.osc_rot(2.6, 0.09, 0, phase=0.1, base=(-0.95, 0, 0))},
        lor:   {"rotation": A.osc_rot(2.6, 0.09, 0, phase=0.8, base=(-0.95, 0, 0))},
        thl:   {"rotation": A.rkeys([(0.0, (-0.50, 0, 0))], 1)},
        thr:   {"rotation": A.rkeys([(0.0, (-0.50, 0, 0))], 1)},
    }
    idle.update(A.chain(TAILC, 2.6, 0.30, 1, lag=0.62, decay=0.86,
                        base=[(-0.26, 0, 0)] + [(-0.16, 0, 0)]*4))
    d.animation("idle_crouch", idle)

    walk = A.biped_walk(0.72, hip, thl, shl, thr, shr, torso, upl, upr,
                        stride=0.92, lift=0.70, bounce=0.045, hip_y=0.60,
                        sway=0.13, digitigrade=0.62)
    walk[neck] = {"rotation": A.osc_rot(0.72, 0.08, 0, base=(-0.30, 0, 0), harm2=0.4)}
    walk[head] = {"rotation": A.osc_rot(0.72, 0.06, 0, phase=1.0, base=(0.22, 0, 0))}
    walk[cnl] = {"rotation": A.osc_rot(0.72, 0.32, 0, phase=0.9, base=(0.80, 0, 0))}
    walk[cnr] = {"rotation": A.osc_rot(0.72, 0.32, 0, phase=0.9+math.pi, base=(0.80, 0, 0))}
    walk.update(A.chain(TAILC, 0.72, 0.34, 1, lag=0.75, decay=0.84,
                        base=[(0.10, 0, 0)] + [(-0.12, 0, 0)]*4))
    d.animation("scamper", walk)

    leap = {
        hip: {"translation": A.tkeys(
            [(0.00, (0, 0.62, 0)), (0.26, (0, 0.36, -0.14)), (0.42, (0, 1.02, 0.75)),
             (0.62, (0, 0.86, 1.45)), (0.80, (0, 0.44, 1.85)), (1.05, (0, 0.62, 1.90))],
            5, [A.ease_in, A.snap, A.linear, A.ease_in, A.overshoot])},
        torso: {"rotation": A.rkeys(
            [(0.00, (0.14, 0, 0)), (0.26, (0.55, 0, 0)), (0.42, (-0.30, 0, 0)),
             (0.62, (0.20, 0, 0)), (0.80, (0.72, 0, 0)), (1.05, (0.14, 0, 0))], 5)},
        jw: {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.40, (0.80, 0, 0)),
                                  (0.80, (0.25, 0, 0)), (1.05, (0, 0, 0))], 4, A.snap)},
    }
    for th, sh in ((thl, shl), (thr, shr)):
        leap[th] = {"rotation": A.rkeys(
            [(0.00, (-0.50, 0, 0)), (0.26, (-1.25, 0, 0)), (0.42, (0.55, 0, 0)),
             (0.62, (-0.65, 0, 0)), (0.80, (-1.30, 0, 0)), (1.05, (-0.50, 0, 0))], 5)}
        leap[sh] = {"rotation": A.rkeys(
            [(0.00, (-0.55, 0, 0)), (0.26, (-1.55, 0, 0)), (0.42, (-0.10, 0, 0)),
             (0.62, (-1.20, 0, 0)), (0.80, (-1.60, 0, 0)), (1.05, (-0.55, 0, 0))], 5)}
    for up, sx in ((upl, -1), (upr, 1)):
        leap[up] = {"rotation": A.rkeys(
            [(0.00, (0.55, 0, 0.62*sx)), (0.26, (1.30, 0, 0.35*sx)),
             (0.42, (-1.85, 0, 0.95*sx)), (0.72, (-1.20, 0, 0.95*sx)),
             (1.05, (0.55, 0, 0.62*sx))], 5)}
    leap.update(A.chain_follow(TAILC,
        [(0.00, (-0.26, 0, 0)), (0.26, (0.45, 0, 0)), (0.46, (-0.55, 0, 0)),
         (0.80, (0.65, 0, 0)), (1.05, (0.30, 0, 0))], 0.09, 4))
    d.animation("leap", leap)

    swipe = {
        hip:   {"translation": A.tkeys([(0.0, (0, 0.62, 0)), (0.20, (0, 0.70, -0.06)),
                                        (0.38, (0, 0.66, 0.14)), (0.75, (0, 0.62, 0))], 4)},
        torso: {"rotation": A.rkeys([(0.0, (0.14, 0, 0)), (0.20, (-0.18, 0.55, 0)),
                                     (0.38, (0.10, -0.62, 0)), (0.75, (0.14, 0, 0))],
                                    5, [A.ease_in, A.snap, A.smooth])},
        upr:   {"rotation": A.rkeys([(0.0, (0.55, 0, -0.55)), (0.20, (-0.55, 1.10, -1.30)),
                                     (0.36, (0.85, -1.25, -0.30)), (0.75, (0.55, 0, -0.55))],
                                    5, [A.ease_in, A.snap, A.smooth])},
        lor:   {"rotation": A.rkeys([(0.0, (-0.95, 0, 0)), (0.20, (-1.45, 0, 0)),
                                     (0.36, (-0.20, 0, 0)), (0.75, (-0.95, 0, 0))], 5, A.snap)},
        hdr:   {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.22, (0, 0.6, 0)),
                                     (0.40, (0, -0.7, 0)), (0.75, (0, 0, 0))], 4)},
        jw:    {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.16, (0.75, 0, 0)),
                                     (0.45, (0.30, 0, 0)), (0.75, (0, 0, 0))], 4, A.snap)},
        neck:  {"rotation": A.rkeys([(0.0, (-0.12, 0, 0)), (0.20, (-0.45, -0.3, 0)),
                                     (0.38, (0.25, 0.35, 0)), (0.75, (-0.12, 0, 0))], 4)},
    }
    swipe.update(A.chain_follow(TAILC,
        [(0.0, (-0.26, 0, 0)), (0.20, (-0.26, 0.85, 0)), (0.40, (-0.26, -0.95, 0)),
         (0.75, (-0.26, 0, 0))], 0.11, 4))
    d.animation("swipe", swipe)

    death = {
        hip:   {"translation": A.tkeys([(0.0, (0, 0.62, 0)), (0.16, (0, 0.68, -0.16)),
                                        (0.50, (0, 0.34, -0.10)), (1.10, (0, 0.16, 0.12))],
                                       5, [A.snap, A.ease_in, A.smooth]),
                "rotation": A.rkeys([(0.0, (0, 0, 0)), (0.16, (-0.35, 0.2, 0.1)),
                                     (0.50, (0.55, 0.1, -0.2)), (1.10, (1.30, 0.25, -0.4))], 5)},
        torso: {"rotation": A.rkeys([(0.0, (0.14, 0, 0)), (0.16, (-0.55, 0, 0)),
                                     (0.55, (0.70, 0, 0.2)), (1.10, (0.95, 0, 0.3))], 5)},
        neck:  {"rotation": A.rkeys([(0.0, (-0.12, 0, 0)), (0.16, (-0.85, 0, 0)),
                                     (1.10, (0.55, 0.3, 0))], 5)},
        jw:    {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.20, (0.85, 0, 0)),
                                     (1.10, (0.55, 0, 0))], 4, A.snap)},
        eyes:  {"scale": A.skeys([(0.0, (1, 1, 1)), (0.18, (1.6, 1.6, 1.6)),
                                  (0.75, (0.15, 0.15, 0.15)), (1.10, (0, 0, 0))],
                                 4, A.ease_in)},
    }
    for up, sx in ((upl, -1), (upr, 1)):
        death[up] = {"rotation": A.rkeys([(0.0, (0.55, 0, 0.62*sx)), (0.20, (-1.5, 0, 1.0*sx)),
                                          (1.10, (0.9, 0, 0.35*sx))], 5)}
    for th, sh in ((thl, shl), (thr, shr)):
        death[th] = {"rotation": A.rkeys([(0.0, (-0.50, 0, 0)), (0.50, (-1.35, 0, 0)),
                                          (1.10, (-1.55, 0, 0))], 5)}
        death[sh] = {"rotation": A.rkeys([(0.0, (-0.55, 0, 0)), (0.50, (-1.70, 0, 0)),
                                          (1.10, (-1.90, 0, 0))], 5)}
    death.update(A.chain_follow(TAILC,
        [(0.0, (-0.26, 0, 0)), (0.16, (-0.75, 0, 0)), (0.60, (0.85, 0.3, 0)),
         (1.10, (0.55, 0.15, 0))], 0.12, 4))
    d.animation("death", death)
    return d, idx, cold, veil, tb, "imp"


# =================================================================== HELLHOUND
def build_hellhound():
    """Quadruped. Bony, horned skull, dorsal spine ridge, long whip tail.
    Veil property: it only MOVES when the veil is down."""
    d = Doc("HELLHOUND")
    idx, cold, veil, tb = make_texture(59, 32, 32, "pelt",
        TX.ramp((0.05, 0.02, 0.02), (0.95, 0.58, 0.40), gamma=1.0))
    mat = d.material("hound_pelt", png=TX.png_bytes(TX.render(idx, veil, 1)),
                     emissive=(0.42, 0.04, 0.03))
    dk, md = (0.14, 0.05, 0.05, 1), (0.55, 0.22, 0.18, 1)
    bone = (0.95, 0.91, 0.82, 1)

    root = d.joint("HELLHOUND")
    hips = d.joint("hips", root, t=(0, 0.64, -0.34))
    hips.mesh = weld(loft([ring(-0.14, 0.17, 5), ring(0.08, 0.20, 5)], [dk, md], 5))
    spine = d.joint("spine", hips, t=(0, 0.05, 0.20))
    sm = lay_fwd(loft([ring(0, 0.18, 6, sx=0.82), ring(-0.24, 0.21, 6, sx=0.82),
                       ring(-0.50, 0.185, 6, sx=0.82)],
                      [md, dk, md], 6))
    sm.merge(mv(P.spine_ridge(6, 0.56, 0.115, 0.045, c0=bone, c1=dk), 0, 0.19, 0.06))
    spine.mesh = weld(sm)
    chest = d.joint("chest", spine, t=(0, 0.02, 0.36))
    chest.mesh = weld(lay_fwd(loft([ring(0, 0.19, 6, sx=0.82), ring(-0.26, 0.235, 6, sx=0.82, sz=0.92)],
                                   [dk, md], 6)))
    neck = d.joint("neck", chest, t=(0, 0.10, 0.20), r=euler(0.55, 0, 0))
    neck.mesh = weld(loft([ring(0, 0.13, 5), ring(0.24, 0.10, 5)], [md, dk], 5))
    skull = d.joint("skull", neck, t=(0, 0.24, 0), r=euler(-0.75, 0, 0))
    km = lay_fwd(loft([ring(0, 0.10, 5), ring(-0.10, 0.115, 5, sz=0.88),
                       ring(-0.20, 0.075, 5, sz=0.80)], [dk, md, md], 5))
    km.merge(mv(lay_fwd(loft([ring(0, 0.075, 4), ring(-0.20, 0.055, 4)], [md, dk], 4)),
                0, -0.015, 0.16))
    km.merge(mv(P.fangs(5, 0.13, 0.07, bone), 0, -0.045, 0.30))
    skull.mesh = weld(km)
    hjaw = d.joint("jaw", skull, t=(0, -0.05, 0.12))
    hjaw.mesh = weld(P.jaw(0.12, 0.20, 0.06, 5, md, bone))
    for s, sx in (("l", -1), ("r", 1)):
        h = d.joint(f"horn_{s}", skull, t=(0.062*sx, 0.055, 0.02),
                    r=euler(-0.85, 0, -0.34*sx))
        h.mesh = weld(P.horn(0.24, 0.034, 0.005, 0.60, 0.5*sx, 4, 3, dk, bone))

    def leg(name, parent, x, z, up_len, lo_len, fwd):
        up = d.joint(f"{name}_up", parent, t=(x, -0.10, z), r=euler(fwd, 0, 0))
        up.mesh = weld(loft([ring(0, 0.088, 4), ring(-up_len, 0.066, 4)], [md, dk], 4))
        lo = d.joint(f"{name}_lo", up, t=(0, -up_len, 0), r=euler(-fwd*1.7, 0, 0))
        lo.mesh = weld(loft([ring(0, 0.064, 4), ring(-lo_len, 0.046, 4)], [dk, md], 4))
        pw = d.joint(f"{name}_paw", lo, t=(0, -lo_len, 0), r=euler(fwd*0.7, 0, 0))
        pm = box(0.085, 0.05, 0.13, md, cy=-0.025)
        pm.merge(mv(P.claw_hand(3, 0.07, 0.36, md, bone), 0, -0.03, 0.05))
        pw.mesh = weld(pm)
        return up, lo, pw

    fl = leg("fore_l", chest, -0.15, 0.04, 0.21, 0.20, 0.30)
    fr = leg("fore_r", chest,  0.15, 0.04, 0.21, 0.20, 0.30)
    hl = leg("hind_l", hips,  -0.16, -0.02, 0.23, 0.22, -0.55)
    hr = leg("hind_r", hips,   0.16, -0.02, 0.23, 0.22, -0.55)

    tail, hb = make_tail(d, hips, 4, 0.20, 0.048, dk, md, (0, 0.04, -0.16),
                         0.14, 0.09, bone, taper=0.70)
    eyes = add_eyes(d, skull, 0.052, 0.015, 0.03, 0.20)
    for j in d.joints:
        if j.mesh and j.mat is None:
            j.mat = mat

    LEGS = [(fl[0], fl[1]), (fr[0], fr[1]), (hl[0], hl[1]), (hr[0], hr[1])]
    TAILC = tail + [hb]

    # FROZEN: the pose it holds while you are looking at it. Almost nothing
    # moves - only the eyes and a shallow breath. The stillness is the horror.
    d.animation("frozen", {
        chest: {"scale": A.breathe(chest, 3.4, 0.022)},
        eyes:  {"scale": A.skeys([(0.0, (1, 1, 1)), (1.7, (1.35, 1.35, 1.35)),
                                  (3.4, (1, 1, 1))], 6)},
        hb:    {"rotation": A.osc_rot(3.4, 0.06, 1)},
    })

    prowl = A.quad_run(1.05, hips, LEGS, spine, reach=0.55, fold=0.42,
                       bound=0.045, base_y=0.64)
    prowl[neck] = {"rotation": A.osc_rot(1.05, 0.07, 0, base=(0.62, 0, 0), harm2=0.4)}
    prowl[skull] = {"rotation": A.osc_rot(1.05, 0.06, 1, phase=1.0, base=(-0.78, 0, 0))}
    prowl.update(A.chain(TAILC, 1.05, 0.22, 1, lag=0.70, decay=0.85,
                          base=[(-0.14, 0, 0)]*5))
    d.animation("prowl", prowl)

    charge = A.quad_run(0.44, hips, LEGS, spine, reach=1.05, fold=0.85,
                        bound=0.16, base_y=0.66)
    charge[neck] = {"rotation": A.osc_rot(0.44, 0.10, 0, base=(0.48, 0, 0))}
    charge[skull] = {"rotation": A.osc_rot(0.44, 0.09, 0, phase=0.8, base=(-0.60, 0, 0))}
    charge[hjaw] = {"rotation": A.osc_rot(0.44, 0.16, 0, base=(0.30, 0, 0))}
    charge.update(A.chain(TAILC, 0.44, 0.30, 1, lag=0.55, decay=0.82,
                          base=[(0.05, 0, 0)]*5))
    d.animation("charge", charge)

    d.animation("bite", {
        hips:  {"translation": A.tkeys([(0.0, (0, 0.64, -0.34)), (0.20, (0, 0.58, -0.48)),
                                        (0.36, (0, 0.74, 0.10)), (0.80, (0, 0.64, -0.34))],
                                       5, [A.ease_in, A.snap, A.smooth])},
        neck:  {"rotation": A.rkeys([(0.0, (0.55, 0, 0)), (0.20, (1.05, 0, 0)),
                                     (0.34, (0.05, 0, 0)), (0.80, (0.55, 0, 0))],
                                    5, [A.ease_in, A.snap, A.smooth])},
        skull: {"rotation": A.rkeys([(0.0, (-0.75, 0, 0)), (0.20, (-1.15, 0, 0)),
                                     (0.34, (-0.20, 0, 0)), (0.80, (-0.75, 0, 0))], 5, A.snap)},
        hjaw:  {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.20, (1.15, 0, 0)),
                                     (0.32, (0.02, 0, 0)), (0.46, (0.55, 0, 0)),
                                     (0.80, (0, 0, 0))], 4, A.snap)},
        spine: {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.20, (-0.30, 0, 0)),
                                     (0.36, (0.35, 0, 0)), (0.80, (0, 0, 0))], 5)},
    })

    d.animation("death", {
        hips:  {"translation": A.tkeys([(0.0, (0, 0.64, -0.34)), (0.18, (0, 0.70, -0.50)),
                                        (0.70, (0, 0.26, -0.40)), (1.30, (0, 0.15, -0.36))],
                                       5, [A.snap, A.ease_in, A.smooth]),
                "rotation": A.rkeys([(0.0, (0, 0, 0)), (0.18, (-0.25, 0, 0)),
                                     (0.70, (0.10, 0.15, 0.85)), (1.30, (0.05, 0.2, 1.25))], 5)},
        spine: {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.18, (-0.45, 0, 0)),
                                     (1.30, (0.35, 0, 0.35))], 5)},
        neck:  {"rotation": A.rkeys([(0.0, (0.55, 0, 0)), (0.18, (0.05, 0, 0)),
                                     (1.30, (1.05, 0.25, 0))], 5)},
        hjaw:  {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.22, (1.0, 0, 0)),
                                     (1.30, (0.62, 0, 0))], 4, A.snap)},
        eyes:  {"scale": A.skeys([(0.0, (1, 1, 1)), (0.16, (1.8, 1.8, 1.8)),
                                  (0.9, (0.12, 0.12, 0.12)), (1.30, (0, 0, 0))], 4, A.ease_in)},
        fl[0]: {"rotation": A.rkeys([(0.0, (0.22, 0, 0)), (0.5, (1.1, 0, 0)),
                                     (1.30, (1.35, 0, 0))], 5)},
        fr[0]: {"rotation": A.rkeys([(0.0, (0.22, 0, 0)), (0.5, (1.0, 0, 0)),
                                     (1.30, (1.30, 0, 0))], 5)},
        hl[0]: {"rotation": A.rkeys([(0.0, (-0.42, 0, 0)), (1.30, (-1.35, 0, 0))], 5)},
        hr[0]: {"rotation": A.rkeys([(0.0, (-0.42, 0, 0)), (1.30, (-1.25, 0, 0))], 5)},
    })
    return d, idx, cold, veil, tb, "hellhound"


# ==================================================================== GARGOYLE
def build_gargoyle():
    """Winged. Veil property: with the veil down it is not drawn, and a real
    statue prop stands in the same spot. Under the veil the shell erases and
    the burning thing inside is revealed."""
    d = Doc("GARGOYLE")
    idx, cold, veil, tb = make_texture(83, 32, 32, "stone",
        TX.ramp((0.08, 0.06, 0.06), (0.88, 0.82, 0.76), gamma=1.15))
    idx2, cold2, veil2, tb2 = make_texture(97, 32, 32, "ember",
        TX.ramp((0.10, 0.01, 0.01), (1.0, 0.72, 0.42), gamma=0.8))
    shell = d.material("gargoyle_shell", png=TX.png_bytes(TX.render(idx, veil, 1)))
    core = d.material("gargoyle_core", png=TX.png_bytes(TX.render(idx2, veil2, 1)),
                      emissive=(0.85, 0.14, 0.06))
    dk, md, ht = (0.14, 0.11, 0.10, 1), (0.55, 0.50, 0.46, 1), (0.90, 0.86, 0.80, 1)

    root = d.joint("GARGOYLE")
    hip = d.joint("hip", root, t=(0, 0.86, 0))
    hip.mesh = weld(loft([ring(-0.10, 0.17, 5), ring(0.08, 0.21, 5)], [dk, md], 5))
    torso = d.joint("torso", hip, t=(0, 0.10, 0))
    torso.mesh = weld(loft([ring(0, 0.22, 6), ring(0.18, 0.29, 6, sz=0.78),
                            ring(0.40, 0.24, 6, sz=0.74), ring(0.52, 0.17, 6)],
                           [md, md, dk, dk], 6))
    coreJ = d.joint("core", torso, t=(0, 0.20, 0.02))
    cm = P.ribs(3, 0.21, 0.20, 0.12, 5, 0.60, (0.20, 0.02, 0.02, 1), (1.0, 0.70, 0.45, 1))
    cm.merge(loft([ring(-0.09, 0.02, 5), ring(0.0, 0.115, 5), ring(0.09, 0.03, 5)],
                  [(1, .8, .5, 1), (1, .55, .25, 1), (0.5, .12, .06, 1)], 5))
    coreJ.mesh, coreJ.mat = weld(cm), core

    neck = d.joint("neck", torso, t=(0, 0.52, 0.02))
    head = d.joint("head", neck, t=(0, 0.08, 0.01))
    hm = loft([ring(-0.09, 0.09, 5), ring(0.02, 0.135, 5, sz=0.9), ring(0.11, 0.07, 5)],
              [dk, md, md], 5)
    hm.merge(mv(loft([ring(0, 0.08, 4), ring(-0.02, 0.06, 4)], [md, dk], 4), 0, 0, 0.11))
    hm.merge(mv(P.fangs(5, 0.13, 0.055), 0, -0.04, 0.15))
    head.mesh = weld(hm)
    gjaw = d.joint("jaw", head, t=(0, -0.06, 0.06))
    gjaw.mesh = weld(P.jaw(0.13, 0.12, 0.07, 4, md, ht))
    for s, sx in (("l", -1), ("r", 1)):
        h = d.joint(f"horn_{s}", head, t=(0.062*sx, 0.085, -0.02),
                    r=euler(-0.55, 0, -0.28*sx))
        h.mesh = weld(P.horn(0.26, 0.042, 0.006, 0.85, 0.0, 4, 3, dk, ht))
        e = d.joint(f"ear_{s}", head, t=(0.105*sx, 0.02, -0.03), r=euler(0, 0, 1.25*sx))
        e.mesh = weld(P.ear(0.14, 0.075, md, dk))

    wings = []
    for s, sx in (("l", -1), ("r", 1)):
        sh = d.joint(f"wing_{s}", torso, t=(0.20*sx, 0.42, -0.09), r=euler(0, 0, -0.45*sx))
        sh.mesh = weld(loft([ring(0, 0.055, 4), ring(-0.10, 0.045, 4)], [md, dk], 4))
        mid = d.joint(f"wingmid_{s}", sh, t=(0, -0.10, 0))
        mid.mesh = weld(loft([ring(0, 0.045, 4), ring(-0.08, 0.035, 4)], [dk, md], 4))
        tip = d.joint(f"wingtip_{s}", mid, t=(0, -0.08, 0))
        tip.mesh = weld(P.bat_wing(0.86, sx, (0.80, 0.74, 0.66, 1),
                                   (0.42, 0.34, 0.30, 1), (0.10, 0.07, 0.06, 1)))
        wings.append((sh, mid, tip))

    arms = []
    for s, sx in (("l", -1), ("r", 1)):
        up = d.joint(f"upperarm_{s}", torso, t=(0.24*sx, 0.40, 0.02), r=euler(0.55, 0, 0.85*sx))
        up.mesh = weld(loft([ring(0, 0.078, 4), ring(-0.28, 0.060, 4)], [md, dk], 4))
        lo = d.joint(f"forearm_{s}", up, t=(0, -0.28, 0), r=euler(-1.05, 0, 0))
        lo.mesh = weld(loft([ring(0, 0.060, 4), ring(-0.26, 0.046, 4)], [dk, md], 4))
        hd = d.joint(f"hand_{s}", lo, t=(0, -0.26, 0))
        hd.mesh = weld(P.claw_hand(3, 0.12, 0.42, md, ht))
        arms.append((up, lo, hd))

    legs = []
    for s, sx in (("l", -1), ("r", 1)):
        tm, sm2, cm2, hm2 = P.digit_leg(0.26, 0.24, 0.15, 0.100, dk, md)
        th = d.joint(f"thigh_{s}", hip, t=(0.115*sx, -0.06, 0.02), r=euler(-0.85, 0, 0))
        th.mesh = weld(tm)
        sh2 = d.joint(f"shin_{s}", th, t=(0, -0.26, 0), r=euler(1.35, 0, 0)); sh2.mesh = weld(sm2)
        cn = d.joint(f"cannon_{s}", sh2, t=(0, -0.24, 0), r=euler(-0.75, 0, 0)); cn.mesh = weld(cm2)
        hf = d.joint(f"hoof_{s}", cn, t=(0, -0.16, 0), r=euler(0.20, 0, 0)); hf.mesh = weld(hm2)
        legs.append((th, sh2, cn, hf))

    tail, gb = make_tail(d, hip, 3, 0.17, 0.048, dk, md, (0, 0, -0.14), 0.15, 0.10, ht)
    eyes = add_eyes(d, head, 0.052, 0.017, 0.02, 0.125)
    for j in d.joints:
        if j.mesh and j.mat is None:
            j.mat = shell

    (wsl, wml, wtl), (wsr, wmr, wtr) = wings

    # PERCH: it is a statue. Nothing moves except a slow ember pulse.
    d.animation("perch_statue", {
        coreJ: {"scale": A.skeys([(0.0, (1, 1, 1)), (1.6, (1.10, 1.10, 1.10)),
                                  (3.2, (1, 1, 1))], 6)},
        eyes:  {"scale": A.skeys([(0.0, (0.9, 0.9, 0.9)), (1.6, (1.25, 1.25, 1.25)),
                                  (3.2, (0.9, 0.9, 0.9))], 6)},
    })

    unfurl = {
        hip:   {"translation": A.tkeys([(0.0, (0, 0.72, 0)), (0.35, (0, 0.68, 0)),
                                        (0.70, (0, 0.92, 0)), (1.20, (0, 0.86, 0))],
                                       5, [A.ease_in, A.overshoot, A.smooth])},
        torso: {"rotation": A.rkeys([(0.0, (0.65, 0, 0)), (0.35, (0.80, 0, 0)),
                                     (0.70, (-0.15, 0, 0)), (1.20, (0.05, 0, 0))],
                                    5, [A.ease_in, A.snap, A.smooth])},
        neck:  {"rotation": A.rkeys([(0.0, (0.75, 0, 0)), (0.45, (0.85, 0, 0)),
                                     (0.72, (-0.55, 0, 0)), (1.20, (-0.10, 0, 0))], 5, A.snap)},
        gjaw:  {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.70, (0.95, 0, 0)),
                                     (1.20, (0.25, 0, 0))], 4, A.snap)},
        coreJ: {"scale": A.skeys([(0.0, (0.6, 0.6, 0.6)), (0.70, (1.5, 1.5, 1.5)),
                                  (1.20, (1.0, 1.0, 1.0))], 5, A.overshoot)},
    }
    for (sh, mid, tip), sx in zip(wings, (-1, 1)):
        unfurl[sh] = {"rotation": A.rkeys(
            [(0.0, (0, 0, -1.55*sx)), (0.40, (0, 0, -1.45*sx)), (0.78, (0, 0, 0.65*sx)),
             (1.20, (0, 0, 0.25*sx))], 6, [A.ease_in, A.snap, A.overshoot])}
        unfurl[mid] = {"rotation": A.rkeys(
            [(0.0, (0, 0, -1.30*sx)), (0.48, (0, 0, -1.10*sx)), (0.86, (0, 0, 0.30*sx)),
             (1.20, (0, 0, 0.10*sx))], 6, A.snap)}
        unfurl[tip] = {"rotation": A.rkeys(
            [(0.0, (0, 0, -0.95*sx)), (0.55, (0, 0, -0.80*sx)), (0.94, (0, 0, 0.20*sx)),
             (1.20, (0, 0, 0.0))], 6, A.snap)}
    d.animation("unfurl", unfurl)

    hover = {
        hip:   {"translation": A.osc_pos(1.0, 0.085, 1, base=(0, 1.02, 0), harm2=0.25)},
        torso: {"rotation": A.osc_rot(1.0, 0.06, 0, phase=0.8, base=(0.10, 0, 0))},
        neck:  {"rotation": A.osc_rot(1.0, 0.05, 0, phase=1.6, base=(-0.12, 0, 0))},
    }
    for (sh, mid, tip), sx in zip(wings, (-1, 1)):
        hover.update(A.flap(1.0, sh, mid, tip, amp=0.85*sx, base=0.30*sx, lag=0.85))
    for th, shn in ((legs[0][0], legs[0][1]), (legs[1][0], legs[1][1])):
        hover[th] = {"rotation": A.osc_rot(1.0, 0.09, 0, phase=0.5, base=(-0.55, 0, 0))}
        hover[shn] = {"rotation": A.osc_rot(1.0, 0.11, 0, phase=1.1, base=(1.05, 0, 0))}
    hover.update(A.chain(tail + [gb], 1.0, 0.24, 1, lag=0.68, decay=0.85,
                        base=[(-0.20, 0, 0)]*4))
    d.animation("hover", hover)

    dive = {
        hip:   {"translation": A.tkeys([(0.0, (0, 1.30, -0.30)), (0.26, (0, 1.55, -0.55)),
                                        (0.60, (0, 0.60, 1.30)), (1.00, (0, 0.90, 1.55))],
                                       6, [A.ease_in, A.snap, A.overshoot])},
        torso: {"rotation": A.rkeys([(0.0, (0.05, 0, 0)), (0.26, (-0.55, 0, 0)),
                                     (0.58, (0.85, 0, 0)), (1.00, (0.15, 0, 0))], 6, A.snap)},
        gjaw:  {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.30, (0.30, 0, 0)),
                                     (0.56, (1.05, 0, 0)), (1.00, (0.15, 0, 0))], 4, A.snap)},
    }
    for (up, lo, hd), sx in zip(arms, (-1, 1)):
        dive[up] = {"rotation": A.rkeys([(0.0, (0.55, 0, 0.85*sx)), (0.26, (1.35, 0, 0.65*sx)),
                                         (0.58, (-1.35, 0, 1.15*sx)), (1.00, (0.55, 0, 0.85*sx))],
                                        6, A.snap)}
    for (sh, mid, tip), sx in zip(wings, (-1, 1)):
        dive[sh] = {"rotation": A.rkeys([(0.0, (0, 0, 0.30*sx)), (0.26, (0, 0, 1.35*sx)),
                                         (0.56, (0, 0, -0.85*sx)), (1.00, (0, 0, 0.30*sx))], 6, A.snap)}
        dive[mid] = {"rotation": A.rkeys([(0.0, (0, 0, 0.10*sx)), (0.30, (0, 0, 1.05*sx)),
                                          (0.60, (0, 0, -0.55*sx)), (1.00, (0, 0, 0.10*sx))], 6, A.snap)}
        dive[tip] = {"rotation": A.rkeys([(0.0, (0, 0, 0.0)), (0.34, (0, 0, 0.75*sx)),
                                          (0.64, (0, 0, -0.35*sx)), (1.00, (0, 0, 0.0))], 6, A.snap)}
    d.animation("dive", dive)

    d.animation("death", {
        hip:   {"translation": A.tkeys([(0.0, (0, 1.02, 0)), (0.20, (0, 1.14, -0.10)),
                                        (0.75, (0, 0.42, -0.05)), (1.40, (0, 0.20, 0.10))],
                                       6, [A.snap, A.ease_in, A.smooth]),
                "rotation": A.rkeys([(0.0, (0, 0, 0)), (0.20, (-0.35, 0.2, 0.15)),
                                     (1.40, (1.15, 0.35, 0.55))], 6)},
        coreJ: {"scale": A.skeys([(0.0, (1, 1, 1)), (0.22, (2.4, 2.4, 2.4)),
                                  (0.85, (0.5, 0.5, 0.5)), (1.40, (0.02, 0.02, 0.02))],
                                 5, A.ease_in)},
        eyes:  {"scale": A.skeys([(0.0, (1, 1, 1)), (0.20, (2.0, 2.0, 2.0)),
                                  (1.00, (0.1, 0.1, 0.1)), (1.40, (0, 0, 0))], 5, A.ease_in)},
        gjaw:  {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.24, (1.05, 0, 0)),
                                     (1.40, (0.65, 0, 0))], 4, A.snap)},
        wsl:   {"rotation": A.rkeys([(0.0, (0, 0, -0.30)), (0.30, (0, 0, -1.35)),
                                     (1.40, (0, 0, -1.65))], 6)},
        wsr:   {"rotation": A.rkeys([(0.0, (0, 0, 0.30)), (0.30, (0, 0, 1.35)),
                                     (1.40, (0, 0, 1.65))], 6)},
    })
    return d, idx, cold, veil, tb, "gargoyle"


# ==================================================================== OVERLORD
def build_overlord():
    """The elite. Two and a half metres of horned brute with a burning crown.
    Veil property: inside its radius the veil is forced ON. It cannot blind
    you, only expose you."""
    d = Doc("OVERLORD")
    idx, cold, veil, tb = make_texture(127, 64, 32, "ember",
        TX.ramp((0.06, 0.01, 0.01), (1.0, 0.66, 0.40), gamma=0.85))
    mat = d.material("overlord_hide", png=TX.png_bytes(TX.render(idx, veil, 1)),
                     emissive=(0.55, 0.06, 0.04))
    crown_mat = d.material("overlord_crown", rgba=(1.0, 0.30, 0.10, 0.75),
                           alpha_mode="BLEND", emissive=(1.0, 0.35, 0.12))
    dk, md = (0.12, 0.04, 0.04, 1), (0.52, 0.20, 0.15, 1)
    bone = (0.96, 0.92, 0.82, 1)

    root = d.joint("OVERLORD")
    hip = d.joint("hip", root, t=(0, 1.32, 0))
    hip.mesh = weld(loft([ring(-0.16, 0.28, 6), ring(0.12, 0.32, 6)], [dk, md], 6))
    torso = d.joint("torso", hip, t=(0, 0.14, 0))
    torso.mesh = weld(loft([ring(0, 0.32, 6), ring(0.24, 0.40, 6, sz=0.72),
                            ring(0.52, 0.44, 6, sz=0.68), ring(0.70, 0.30, 6)],
                           [md, md, dk, dk], 6))
    rib = d.joint("ribs", torso, t=(0, 0.34, 0.10))
    rib.mesh = weld(P.ribs(3, 0.30, 0.20, 0.14, 5, 0.52, dk, bone))
    neck = d.joint("neck", torso, t=(0, 0.70, 0.0))
    head = d.joint("head", neck, t=(0, 0.10, 0.02))
    hm = loft([ring(-0.12, 0.13, 5), ring(0.0, 0.19, 5, sz=0.88), ring(0.14, 0.10, 5)],
              [dk, md, md], 5)
    hm.merge(mv(loft([ring(0, 0.11, 4), ring(-0.03, 0.085, 4)], [md, dk], 4), 0, 0, 0.15))
    hm.merge(mv(P.fangs(6, 0.19, 0.085, bone), 0, -0.055, 0.21))
    head.mesh = weld(hm)
    ojaw = d.joint("jaw", head, t=(0, -0.08, 0.08))
    ojaw.mesh = weld(P.jaw(0.19, 0.17, 0.10, 5, md, bone))
    for s, sx in (("l", -1), ("r", 1)):
        # huge ram horns - the whole silhouette
        h = d.joint(f"horn_{s}", head, t=(0.10*sx, 0.09, -0.02), r=euler(-0.20, 0, -0.85*sx))
        h.mesh = weld(P.horn(0.62, 0.085, 0.010, 1.05, 1.5*sx, 5, 4, dk, bone))

    crown = d.joint("crown", head, t=(0, 0.20, 0))
    cm = Mesh()
    for i in range(9):
        a = TAU * i / 9
        f = blade(0.20, 0.06, 0.004, (1.0, 0.55, 0.20, 1),
                  (0.55, 0.06, 0.03, 1), steps=1)
        f.transform(lambda p, a=a: (math.cos(a)*(0.17 + p[0]), -p[1],
                                    math.sin(a)*(0.17 + p[0])))
        cm.merge(f)
    crown.mesh, crown.mat = weld(cm), crown_mat

    arms = []
    for s, sx in (("l", -1), ("r", 1)):
        sj = d.joint(f"shoulder_{s}", torso, t=(0.34*sx, 0.56, 0))
        sj.mesh = weld(loft([ring(0.08, 0.14, 5), ring(-0.06, 0.17, 5), ring(-0.18, 0.12, 5)],
                            [dk, md, dk], 5))
        up = d.joint(f"upperarm_{s}", sj, t=(0.10*sx, -0.14, 0), r=euler(0.20, 0, 0.50*sx))
        up.mesh = weld(loft([ring(0, 0.11, 5), ring(-0.42, 0.085, 5)], [md, dk], 5))
        lo = d.joint(f"forearm_{s}", up, t=(0, -0.42, 0), r=euler(-0.55, 0, 0))
        lo.mesh = weld(loft([ring(0, 0.090, 5), ring(-0.40, 0.062, 5)], [dk, md], 5))
        hd = d.joint(f"hand_{s}", lo, t=(0, -0.40, 0))
        hd.mesh = weld(P.claw_hand(3, 0.22, 0.44, md, bone))
        arms.append((sj, up, lo, hd))

    legs = []
    for s, sx in (("l", -1), ("r", 1)):
        tm, sm2, cm2, hm2 = P.digit_leg(0.44, 0.42, 0.30, 0.135, dk, md)
        th = d.joint(f"thigh_{s}", hip, t=(0.19*sx, -0.12, 0.01)); th.mesh = weld(tm)
        sh2 = d.joint(f"shin_{s}", th, t=(0, -0.44, 0), r=euler(-0.42, 0, 0)); sh2.mesh = weld(sm2)
        cn = d.joint(f"cannon_{s}", sh2, t=(0, -0.42, 0), r=euler(0.62, 0, 0)); cn.mesh = weld(cm2)
        hf = d.joint(f"hoof_{s}", cn, t=(0, -0.30, 0), r=euler(-0.20, 0, 0)); hf.mesh = weld(hm2)
        legs.append((th, sh2, cn, hf))

    tail, ob = make_tail(d, hip, 4, 0.30, 0.075, dk, md, (0, 0, -0.22), 0.26, 0.17, bone)
    eyes = add_eyes(d, head, 0.075, 0.024, 0.02, 0.175)
    for j in d.joints:
        if j.mesh and j.mat is None:
            j.mat = mat

    (sjl, upl, lol, hdl), (sjr, upr, lor, hdr) = arms
    (thl, shl, cnl, hfl), (thr, shr, cnr, hfr) = legs
    TAILC = tail + [ob]

    idle = {
        hip:   {"translation": A.osc_pos(3.4, 0.030, 1, base=(0, 1.32, 0), harm2=0.30)},
        torso: {"scale": A.breathe(torso, 3.4, 0.050),
                "rotation": A.osc_rot(3.4, 0.045, 1, base=(0.05, 0, 0))},
        rib:   {"scale": A.breathe(rib, 3.4, 0.075, phase=0.4)},
        neck:  {"rotation": A.osc_rot(3.4, 0.06, 1, phase=1.2, base=(-0.08, 0, 0))},
        crown: {"rotation": [(0.0, euler(0, 0, 0)), (1.7, euler(0, math.pi, 0)),
                             (3.4, euler(0, TAU - 0.001, 0))],
                "scale": A.skeys([(0.0, (1, 1, 1)), (1.1, (1.22, 1.10, 1.22)),
                                  (2.3, (0.92, 0.96, 0.92)), (3.4, (1, 1, 1))], 5)},
        upl:   {"rotation": A.osc_rot(3.4, 0.055, 0, phase=0.5, base=(0.20, 0, -0.50))},
        upr:   {"rotation": A.osc_rot(3.4, 0.055, 0, phase=1.9, base=(0.20, 0, 0.50))},
        ojaw:  {"rotation": A.rkeys([(0.0, (0, 0, 0)), (2.0, (0.05, 0, 0)),
                                     (2.5, (0.28, 0, 0)), (3.4, (0, 0, 0))], 4)},
    }
    idle.update(A.chain(TAILC, 3.4, 0.26, 1, lag=0.60, decay=0.86,
                        base=[(-0.20, 0, 0)] + [(-0.14, 0, 0)]*4))
    d.animation("idle_smolder", idle)

    stomp = A.biped_walk(1.30, hip, thl, shl, thr, shr, torso, upl, upr,
                         stride=0.62, lift=0.42, bounce=0.075, hip_y=1.32,
                         sway=0.16, digitigrade=0.48)
    stomp[neck] = {"rotation": A.osc_rot(1.30, 0.07, 1, base=(-0.08, 0, 0), harm2=0.4)}
    stomp[cnl] = {"rotation": A.osc_rot(1.30, 0.26, 0, phase=0.8, base=(0.62, 0, 0))}
    stomp[cnr] = {"rotation": A.osc_rot(1.30, 0.26, 0, phase=0.8+math.pi, base=(0.62, 0, 0))}
    stomp[crown] = {"rotation": [(0.0, euler(0, 0, 0)), (0.65, euler(0, math.pi, 0)),
                                 (1.30, euler(0, TAU - 0.001, 0))]}
    stomp.update(A.chain(TAILC, 1.30, 0.24, 1, lag=0.66, decay=0.85,
                         base=[(-0.18, 0, 0)]*5))
    d.animation("stomp", stomp)

    # ROAR: this is the veil_impose beat. Head back, arms wide, crown flares.
    roar = {
        hip:   {"translation": A.tkeys([(0.0, (0, 1.32, 0)), (0.30, (0, 1.18, -0.10)),
                                        (0.60, (0, 1.46, 0.06)), (1.70, (0, 1.32, 0))],
                                       6, [A.ease_in, A.overshoot, A.smooth])},
        torso: {"rotation": A.rkeys([(0.0, (0.05, 0, 0)), (0.30, (0.45, 0, 0)),
                                     (0.60, (-0.42, 0, 0)), (1.70, (0.05, 0, 0))],
                                    6, [A.ease_in, A.snap, A.smooth])},
        neck:  {"rotation": A.rkeys([(0.0, (-0.08, 0, 0)), (0.30, (0.42, 0, 0)),
                                     (0.62, (-0.85, 0, 0)), (1.70, (-0.08, 0, 0))], 6, A.snap)},
        ojaw:  {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.30, (0.12, 0, 0)),
                                     (0.60, (1.15, 0, 0)), (1.25, (0.95, 0, 0)),
                                     (1.70, (0, 0, 0))], 5, A.snap)},
        crown: {"scale": A.skeys([(0.0, (1, 1, 1)), (0.30, (0.72, 1.2, 0.72)),
                                  (0.66, (3.6, 1.6, 3.6)), (1.25, (2.2, 1.2, 2.2)),
                                  (1.70, (1, 1, 1))], 6,
                                 [A.ease_in, A.snap, A.smooth, A.smooth])},
        rib:   {"scale": A.skeys([(0.0, (1, 1, 1)), (0.60, (1.30, 1.18, 1.30)),
                                  (1.70, (1, 1, 1))], 5, A.snap)},
    }
    for (sj, up, lo, hd), sx in zip(arms, (-1, 1)):
        roar[up] = {"rotation": A.rkeys([(0.0, (0.20, 0, 0.50*sx)), (0.30, (0.75, 0, 0.25*sx)),
                                         (0.62, (-0.35, 0, 2.15*sx)), (1.70, (0.20, 0, 0.50*sx))],
                                        6, A.snap)}
        roar[lo] = {"rotation": A.rkeys([(0.0, (-0.55, 0, 0)), (0.30, (-1.35, 0, 0)),
                                         (0.62, (-0.30, 0, 0)), (1.70, (-0.55, 0, 0))], 6, A.snap)}
    roar.update(A.chain_follow(TAILC,
        [(0.0, (-0.20, 0, 0)), (0.30, (0.42, 0, 0)), (0.66, (-0.80, 0, 0)),
         (1.70, (-0.20, 0, 0))], 0.10, 5))
    d.animation("roar", roar)

    cleave = {
        hip:   {"translation": A.tkeys([(0.0, (0, 1.32, 0)), (0.34, (0, 1.40, -0.16)),
                                        (0.56, (0, 1.10, 0.30)), (1.20, (0, 1.32, 0))],
                                       6, [A.ease_in, A.snap, A.smooth])},
        torso: {"rotation": A.rkeys([(0.0, (0.05, 0, 0)), (0.34, (-0.40, 0.62, 0)),
                                     (0.56, (0.62, -0.70, 0)), (1.20, (0.05, 0, 0))],
                                    6, [A.ease_in, A.snap, A.smooth])},
        sjr:   {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.34, (0, 0.5, 0)),
                                     (0.56, (0, -0.45, 0)), (1.20, (0, 0, 0))], 5)},
        upr:   {"rotation": A.rkeys([(0.0, (0.20, 0, 0.50)), (0.34, (-1.85, 0.4, 1.05)),
                                     (0.56, (1.15, -0.5, 0.25)), (1.20, (0.20, 0, 0.50))],
                                    6, [A.ease_in, A.snap, A.smooth])},
        lor:   {"rotation": A.rkeys([(0.0, (-0.55, 0, 0)), (0.34, (-1.55, 0, 0)),
                                     (0.56, (-0.12, 0, 0)), (1.20, (-0.55, 0, 0))], 6, A.snap)},
        hdr:   {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.36, (0, 0.7, 0)),
                                     (0.58, (0, -0.8, 0)), (1.20, (0, 0, 0))], 5)},
        upl:   {"rotation": A.rkeys([(0.0, (0.20, 0, -0.50)), (0.34, (0.85, 0, -1.05)),
                                     (0.56, (-0.35, 0, -0.30)), (1.20, (0.20, 0, -0.50))], 6, A.snap)},
        ojaw:  {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.30, (0.85, 0, 0)),
                                     (0.70, (0.30, 0, 0)), (1.20, (0, 0, 0))], 4, A.snap)},
        thl:   {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.34, (0.28, 0, 0)),
                                     (0.56, (-0.35, 0, 0)), (1.20, (0, 0, 0))], 5)},
        thr:   {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.34, (-0.30, 0, 0)),
                                     (0.56, (0.30, 0, 0)), (1.20, (0, 0, 0))], 5)},
    }
    cleave.update(A.chain_follow(TAILC,
        [(0.0, (-0.20, 0, 0)), (0.34, (-0.20, 0.95, 0)), (0.58, (-0.20, -1.05, 0)),
         (1.20, (-0.20, 0, 0))], 0.12, 5))
    d.animation("cleave", cleave)

    d.animation("death", {
        hip:   {"translation": A.tkeys([(0.0, (0, 1.32, 0)), (0.22, (0, 1.42, -0.18)),
                                        (0.90, (0, 0.72, -0.05)), (1.90, (0, 0.36, 0.22))],
                                       6, [A.snap, A.ease_in, A.smooth]),
                "rotation": A.rkeys([(0.0, (0, 0, 0)), (0.22, (-0.35, 0.15, 0.1)),
                                     (0.90, (0.65, 0.2, -0.30)), (1.90, (1.35, 0.3, -0.55))], 6)},
        torso: {"rotation": A.rkeys([(0.0, (0.05, 0, 0)), (0.22, (-0.55, 0, 0)),
                                     (1.00, (0.75, 0, 0.2)), (1.90, (0.95, 0, 0.35))], 6)},
        neck:  {"rotation": A.rkeys([(0.0, (-0.08, 0, 0)), (0.22, (-0.85, 0, 0)),
                                     (1.90, (0.75, 0.3, 0))], 6)},
        ojaw:  {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.26, (1.05, 0, 0)),
                                     (1.90, (0.70, 0, 0))], 4, A.snap)},
        crown: {"scale": A.skeys([(0.0, (1, 1, 1)), (0.26, (3.0, 1.4, 3.0)),
                                  (1.10, (0.6, 0.8, 0.6)), (1.90, (0.02, 0.02, 0.02))],
                                 6, A.ease_in)},
        rib:   {"scale": A.skeys([(0.0, (1, 1, 1)), (0.26, (1.45, 1.30, 1.45)),
                                  (1.90, (0.85, 0.85, 0.85))], 5, A.ease_in)},
        eyes:  {"scale": A.skeys([(0.0, (1, 1, 1)), (0.24, (2.2, 2.2, 2.2)),
                                  (1.30, (0.1, 0.1, 0.1)), (1.90, (0, 0, 0))], 5, A.ease_in)},
        upl:   {"rotation": A.rkeys([(0.0, (0.20, 0, -0.50)), (0.30, (-1.55, 0, -0.95)),
                                     (1.90, (1.05, 0, -0.30))], 6)},
        upr:   {"rotation": A.rkeys([(0.0, (0.20, 0, 0.50)), (0.30, (-1.45, 0, 0.95)),
                                     (1.90, (1.00, 0, 0.30))], 6)},
        thl:   {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.90, (-0.95, 0, 0)),
                                     (1.90, (-1.35, 0, 0))], 6)},
        thr:   {"rotation": A.rkeys([(0.0, (0, 0, 0)), (0.90, (-0.85, 0, 0)),
                                     (1.90, (-1.25, 0, 0))], 6)},
        shl:   {"rotation": A.rkeys([(0.0, (-0.42, 0, 0)), (1.90, (-1.85, 0, 0))], 6)},
        shr:   {"rotation": A.rkeys([(0.0, (-0.42, 0, 0)), (1.90, (-1.75, 0, 0))], 6)},
    })
    return d, idx, cold, veil, tb, "overlord"


BUILDERS = (build_imp, build_hellhound, build_gargoyle, build_overlord)


# ========================================================================= MAIN
def report(doc, name, tex_bytes):
    tris, verts, parts = doc.stats()
    over = [p for p in parts if p[2] > 32]
    keys = sum(len(k) for _, tr in doc.animations for ch in tr.values()
               for k in ch.values())
    print(f"\n  {name}")
    print(f"    tris {tris:4d}  verts {verts:4d}  parts {len(parts):2d}  "
          f"anims {len(doc.animations)}  keyframes {keys}")
    print(f"    TMEM {tex_bytes} B tex + 64 B TLUT = {tex_bytes+64} / 4096   "
          f"|  {len(over)} part(s) need 2 gSPVertex loads")
    return tris


def main():
    total = 0
    hdr = ["/* veil_tluts.h - generated. PHANTOM cold + veil palettes, RGBA5551.",
           " * Every cold entry has alpha 0: with alpha compare on, nothing draws. */",
           "#ifndef VEIL_TLUTS_H", "#define VEIL_TLUTS_H", "#include <stdint.h>", ""]
    print("VEIL bestiary")
    for fn in BUILDERS:
        doc, idx, cold, veil, tb, slug = fn()
        doc.save_glb(os.path.join(OUT, f"{slug}.glb"))
        total += report(doc, slug.upper(), tb)
        TX.render(idx, veil, 8).save(os.path.join(OUT, f"{slug}_veil_on.png"))
        TX.tlut_strip(cold, veil).save(os.path.join(OUT, f"{slug}_tluts.png"))
        with open(os.path.join(OUT, f"{slug}.ci4"), "wb") as f:
            f.write(TX.ci4_blob(idx))
        hdr.append(TX.c_tlut(f"tlut_{slug}_cold", cold))
        hdr.append(TX.c_tlut(f"tlut_{slug}_veil", veil))
    env = TX.ramp((0.10, 0.12, 0.14), (0.62, 0.66, 0.70))
    hdr.append(TX.c_tlut("tlut_env_cold", env))
    hdr.append(TX.c_tlut("tlut_env_veil", TX.veil_project(env, TX.W_BAND)))
    hdr.append(TX.c_tlut("tlut_eye_tell",
                         TX.eye_cold(TX.ramp((0.1, 0, 0), (1, 0.7, 0.5)))))
    hdr.append("#endif\n")
    with open(os.path.join(OUT, "veil_tluts.h"), "w") as f:
        f.write("\n".join(hdr))
    print(f"\n  TOTAL {total} tris across 4 demons")


if __name__ == "__main__":
    main()
