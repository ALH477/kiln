"""
parts.py - classic demon anatomy at N64 triangle counts.

Everything here is built to be cheap and to read in SILHOUETTE, because under
the veil silhouette and value are the only channels you have. Horns, ears,
wings and tails all exist to make the outline unmistakable at 30 pixels tall.
"""
import math
from gltfkit import Mesh, ring, loft, box, blade

TAU = math.pi * 2


def horn(length=0.34, r0=0.075, r1=0.006, curve=0.55, twist=0.0, segs=5,
         rings=4, c0=(0.30, 0.26, 0.24, 1), c1=(0.94, 0.90, 0.80, 1),
         ridges=True):
    """Curved tapered horn. `curve` sweeps it back, `twist` corkscrews it."""
    R, C = [], []
    for k in range(rings + 1):
        t = k / rings
        r = r0 + (r1 - r0) * (t ** 0.8)
        if ridges and k % 2 == 1:
            r *= 1.16                      # knuckled growth rings, free detail
        y = length * t
        z = curve * length * (t ** 1.7)
        ph = twist * t
        rg = [(math.cos(ph + TAU*i/segs) * r,
               y,
               math.sin(ph + TAU*i/segs) * r + z) for i in range(segs)]
        R.append(rg)
        C.append(tuple(a + (b - a) * t for a, b in zip(c0, c1)))
    return loft(R, C, segs, cap_bottom=False, cap_top=False)


def tail_seg(length, r0, r1, segs=4, c0=(0.30,)*3+(1,), c1=(0.55,)*3+(1,)):
    return loft([ring(0, r0, segs), ring(-length, r1, segs)], [c0, c1], segs,
                cap_bottom=False, cap_top=False)


def barb(length=0.18, w=0.11, c0=(0.9, 0.85, 0.75, 1), c1=(0.25, 0.20, 0.18, 1)):
    """Arrowhead tail tip - the single most legible demon cue in a silhouette."""
    m = Mesh()
    pts = [(0, 0, 0), (-w/2, -length*0.45, 0), (0, -length, 0), (w/2, -length*0.45, 0)]
    uv = [(0.5, 0), (0, 0.5), (0.5, 1), (1, 0.5)]
    m.add_poly(pts, uv, [c0, c1, c1, c1])
    m.add_poly(pts[::-1], uv[::-1], [c1, c1, c1, c0])
    return m


def bat_wing(span=1.0, side=1, c_bone=(0.72, 0.66, 0.58, 1),
             c_mem=(0.42, 0.16, 0.16, 1), c_edge=(0.12, 0.06, 0.06, 1)):
    """
    Membrane between three fingers, plus the finger struts. Built in the wing's
    own local space so the whole thing hangs off one joint; the finger splay is
    baked, and the joint animates the beat.
    """
    m = Mesh()
    s = side
    # finger tips, fanned back and down
    tips = [(0.98*span*s, 0.10*span, -0.10*span),
            (0.86*span*s, -0.26*span, -0.34*span),
            (0.54*span*s, -0.52*span, -0.52*span)]
    root = (0.06*span*s, 0.0, 0.0)
    body = (0.0, -0.30*span, -0.30*span)      # where the membrane meets the ribs
    # membrane panels
    quads = [(root, tips[0], tips[1]), (tips[1], tips[0], tips[2]),
             (tips[1], tips[2], body)]
    for a, b, c in quads:
        for flip in (False, True):
            m.add_poly([a, b, c], [(0, 0), (1, 0), (1, 1)],
                       [c_mem, c_edge, c_mem], flip=flip)
    # finger struts - thin two-sided triangles from the wrist to each tip.
    # 2 tris per finger; they catch the light and sell the anatomy for almost
    # nothing.
    for i, tp in enumerate(tips):
        w = 0.055 - i * 0.010
        a = (root[0], root[1] + w, root[2])
        b = (root[0], root[1] - w, root[2])
        for flip in (False, True):
            m.add_poly([a, b, tp], [(0, 0), (0, 1), (1, 0.5)],
                       [c_bone, c_edge, c_bone], flip=flip)
    return m


def digit_leg(thigh_len=0.34, shin_len=0.32, cannon_len=0.26, r=0.085,
              c_dark=(0.26,)*3+(1,), c_mid=(0.60,)*3+(1,)):
    """Returns (thigh_mesh, shin_mesh, cannon_mesh, hoof_mesh) for a goat leg."""
    th = loft([ring(0, r*1.35, 5), ring(-thigh_len*0.55, r*1.15, 5),
               ring(-thigh_len, r*0.80, 5)], [c_mid, c_mid, c_dark], 5)
    sh = loft([ring(0, r*0.80, 5), ring(-shin_len, r*0.52, 5)],
              [c_dark, c_mid], 5)
    cn = loft([ring(0, r*0.44, 4), ring(-cannon_len, r*0.34, 4)],
              [c_mid, c_dark], 4)
    hf = Mesh()
    hf.merge(loft([ring(0, r*0.40, 4), ring(-0.05, r*0.52, 4),
                   ring(-0.13, r*0.40, 4)],
                  [c_dark, (0.9, 0.86, 0.78, 1), (0.55, 0.50, 0.45, 1)], 4))
    return th, sh, cn, hf


def jaw(width=0.16, length=0.16, depth=0.10, teeth=4,
        c=(0.34, 0.20, 0.18, 1), c_t=(0.96, 0.93, 0.84, 1)):
    m = Mesh()
    # jaw as a wedge: 4 triangles instead of a 12-triangle box
    hw, hl = width/2, length/2
    m.add_poly([(-hw,0,-hl),(hw,0,-hl),(hw,0,hl),(-hw,0,hl)],
               [(0,0),(1,0),(1,1),(0,1)], [c]*4, flip=True)
    m.add_poly([(-hw,-depth*0.7,-hl*0.7),(-hw,0,hl),(hw,0,hl),
                (hw,-depth*0.7,-hl*0.7)], [(0,0),(1,0),(1,1),(0,1)], [c]*4)
    for i in range(teeth):
        u = (i + 0.5) / teeth - 0.5
        t = blade(0.055, 0.030, 0.004, c_t, (0.7, 0.66, 0.6, 1), steps=1)
        t.transform(lambda p, u=u: (p[0] + u*width*0.86, p[1] + depth*0.02,
                                    p[2] + length*0.42))
        m.merge(t)
    return m


def fangs(n=4, width=0.17, length=0.06, c=(0.98, 0.95, 0.86, 1)):
    """Upper fangs, hanging down from a snout."""
    m = Mesh()
    for i in range(n):
        u = (i + 0.5) / n - 0.5
        t = blade(length, 0.030, 0.004, c, (0.6, 0.56, 0.5, 1), steps=1)
        t.transform(lambda p, u=u: (p[0] + u*width, p[1], p[2]))
        m.merge(t)
    return m


def ear(length=0.17, w=0.09, c0=(0.52, 0.24, 0.22, 1), c1=(0.16, 0.08, 0.08, 1)):
    m = blade(length, w, 0.012, c0, c1, curve=0.05, steps=2)
    return m


def claw_hand(fingers=3, length=0.13, spread=0.42,
              c_p=(0.44, 0.22, 0.20, 1), c_c=(0.95, 0.92, 0.84, 1)):
    m = Mesh()
    # palm as a 4-triangle wedge, not a 12-triangle box
    m.add_poly([(-0.05,0,-0.045),(0.05,0,-0.045),(0.05,-0.05,0.045),
                (-0.05,-0.05,0.045)], [(0,0),(1,0),(1,1),(0,1)], [c_p]*4)
    m.add_poly([(-0.05,-0.05,0.045),(0.05,-0.05,0.045),(0.05,0,-0.045),
                (-0.05,0,-0.045)], [(0,0),(1,0),(1,1),(0,1)], [c_p]*4)
    for i in range(fingers):
        a = (i - (fingers - 1) / 2) * spread
        c = blade(length, 0.038, 0.005, c_p, c_c, curve=0.045, steps=2)
        c.transform(lambda p, a=a: (p[0]*math.cos(a) - p[1]*math.sin(a)*0,
                                    p[1] - 0.03,
                                    p[2] + math.sin(a)*0.05 + p[0]*math.sin(a)*0.6))
        m.merge(c)
    return m


def eye_pair(sep=0.075, r=0.020, y=0.0, z=0.0, c=(1.0, 1.0, 1.0, 1)):
    """
    Two tiny quads. This is the ONLY thing that renders when the veil is down -
    the tell. Four triangles of 'something is in here with you'.
    """
    m = Mesh()
    for sx in (-1, 1):
        cx = sep * sx
        m.add_poly([(cx - r, y - r, z), (cx + r, y - r, z),
                    (cx + r, y + r, z), (cx - r, y + r, z)],
                   [(0, 0), (1, 0), (1, 1), (0, 1)], [c] * 4)
    return m


def ribs(n=4, r=0.28, y0=0.26, dy=0.13, segs=6, squash=0.66,
         c0=(0.16, 0.14, 0.12, 1), c1=(0.95, 0.92, 0.84, 1)):
    m = Mesh()
    for k in range(n):
        y = y0 - k * dy
        rr = r - abs(k - (n - 1) * 0.45) * 0.040
        m.merge(loft([ring(y - 0.026, rr, segs, sz=squash),
                      ring(y + 0.026, rr * 0.97, segs, sz=squash)],
                     [c0, c1], segs))
    return m


def spine_ridge(n=6, length=1.0, h0=0.10, h1=0.04, w=0.03,
                c0=(0.90, 0.86, 0.76, 1), c1=(0.20, 0.16, 0.14, 1)):
    """Row of dorsal spikes. Two triangles each, enormous silhouette value."""
    m = Mesh()
    for i in range(n):
        t = i / max(1, n - 1)
        h = h0 + (h1 - h0) * t
        z = -length * t
        p = [(0, 0, z + 0.05), (0, h, z), (0, 0, z - 0.05)]
        for flip in (False, True):
            m.add_poly(p, [(0, 0), (0.5, 1), (1, 0)], [c1, c0, c1], flip=flip)
    return m
