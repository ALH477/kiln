import numpy as np, os
from PIL import Image, ImageDraw
from n64lib import Mesh, box, cyl, blob, blob_uv, render, turntable
from build_melted import melt_warp
from build_cousins import m16, shotgun, pistol, label, sheet

OUT = "/mnt/user-data/outputs"
SKIN   = (220, 202, 158)
SKIN_D = (182, 162, 118)
SKIN_W = (204, 190, 132)
GUM    = ( 96,  58,  58)
TOOTH  = (238, 232, 206)
DCU    = [(222, 204, 158), (196, 174, 124), (128, 104, 66), (122, 122, 82), (44, 44, 42)]
VEST   = ( 92,  96,  62)

SS = 4          # paint oversampled, then crunch down to 64x64


# ---------------------------------------------------------------- textures
# an explicit 16-entry TLUT -- this IS the CI4 palette, so contrast survives
TLUT = [(222,204,164),(186,164,120),(150,128,92),(208,196,138),(238,226,192),
        ( 40, 34, 32),( 16, 14, 18),(242,238,226),(232,226,200),( 92, 44, 48),
        (178, 88, 94),(110, 80, 70),( 98, 80,116),(216,202,140),(140,112, 80),
        (200,150,120)]
P0,P1,P2,P3,P4,INK,PUP,SCL,TTH,GUM_,TNG,NOS,STMP,DRP,MID,BURN = TLUT


def paint_face(seed=1, N=64):
    """The whole melt, minus the geometry. 64x64, front of the head at u=0.5."""
    rng = np.random.default_rng(seed + 900)
    lean = 1 if rng.random() > .5 else -1
    S = N * SS
    im = Image.new("RGB", (S, S), P0)
    d = ImageDraw.Draw(im)

    K = 1.28                                      # fills the decal, front hemisphere only

    def ell(u, v, ru, rv, col, rot=0.0, ink=None):
        u, ru = .5 + (u - .5) * K, ru * K
        cx, cy = u * S, v * S
        pts = [(cx + ru * S * np.cos(t) * np.cos(rot) - rv * S * np.sin(t) * np.sin(rot),
                cy + ru * S * np.cos(t) * np.sin(rot) + rv * S * np.sin(t) * np.cos(rot))
               for t in np.linspace(0, 2 * np.pi, 28)]
        if ink:
            d.polygon([(x + (x - cx) * .16, y + (y - cy) * .16) for x, y in pts], fill=ink)
        d.polygon(pts, fill=col)

    F = .5
    d.rectangle([0, 0, S, .13 * S], fill=BURN)    # sunburnt crown
    d.rectangle([0, .74 * S, S, S], fill=P3)      # everything below has run
    for _ in range(70):                           # blotchy, unwell
        ell(rng.random(), rng.random(), rng.uniform(.006, .022),
            rng.uniform(.005, .016), P1 if rng.random() < .5 else P3)
    for k in range(30):                           # sag pooling toward the jaw
        ell(F + .03 * lean, .44 + .011 * k, .30 - .006 * k, .016, P3)

    # heavy brow shelf, over the good eye only
    ell(F - .105 * lean, .268, .130, .042, P2, rot=.22 * lean)
    ell(F - .105 * lean, .250, .120, .026, P1, rot=.22 * lean)
    # eye 1: big, lidded, locked on you
    ell(F - .098 * lean, .340, .086, .066, P2)
    ell(F - .098 * lean, .348, .066, .048, SCL, ink=INK)
    ell(F - .080 * lean, .352, .030, .030, MID)
    ell(F - .080 * lean, .352, .016, .016, PUP)
    ell(F - .098 * lean, .312, .072, .026, P1, ink=INK)          # lid hanging over it
    # eye 2: slid onto the cheek, dragging a smear of face with it
    for k in range(8):
        ell(F + .100 * lean + .006 * k, .372 + .016 * k, .034 - .002 * k, .017, P1)
    ell(F + .146 * lean, .500, .048, .040, P2)
    ell(F + .146 * lean, .504, .032, .026, SCL, ink=INK)
    ell(F + .152 * lean, .508, .016, .016, PUP)
    for k in range(6):                                            # weeping
        ell(F + .150 * lean, .540 + .016 * k, .012 - .001 * k, .013, DRP)
    # nose: no bridge at all, one hanging bulb
    ell(F + .014 * lean, .455, .046, .062, P1)
    ell(F + .014 * lean, .505, .040, .034, P2, ink=INK)
    ell(F - .014 * lean, .508, .013, .010, NOS)
    ell(F + .044 * lean, .516, .014, .011, NOS)
    # mouth: melted open on the diagonal, and it is not closing again
    ell(F + .012 * lean, .620, .148, .062, P2, rot=.17 * lean)
    ell(F + .012 * lean, .626, .124, .046, GUM_, rot=.17 * lean, ink=INK)
    ell(F - .006 * lean, .646, .062, .026, TNG, rot=.17 * lean)
    for (tu, tv, tw) in [(-.086, .594, .022), (-.032, .602, .019),
                         (.026, .614, .023), (.082, .632, .017)]:
        ell(F + tu * lean, tv, tw, .021, TTH, ink=INK)
    ell(F - .018 * lean, .672, .016, .019, TTH, ink=INK)          # stray lower
    # runnels off the jaw
    for u0 in (-.24, -.10, .05, .20):
        for k in range(7):
            ell(F + u0 * lean, .700 + .019 * k, .020 - .002 * k, .015, DRP)
        ell(F + u0 * lean, .834, .013, .019, DRP)
        ell(F + u0 * lean, .834, .007, .011, P1)
    # batch stamp, sagging with the rest of him
    d.polygon([((F - .235) * S, .225 * S), ((F - .175) * S, .235 * S),
               ((F - .180) * S, .305 * S), ((F - .240) * S, .292 * S)], fill=STMP)
    return im.resize((N, N), Image.BILINEAR)


def paint_body(N=64):
    """Chocolate-chip camo with the flak vest and pouches baked in."""
    rng = np.random.default_rng(4)
    S = N * SS
    im = Image.new("RGB", (S, S), DCU[0])
    d = ImageDraw.Draw(im)
    for _ in range(220):                                  # camo blotches
        x, y = rng.random() * S, rng.random() * S
        w, h = rng.uniform(.04, .16) * S, rng.uniform(.03, .11) * S
        d.ellipse([x, y, x + w, y + h], fill=DCU[rng.integers(1, 4)])
    for _ in range(70):                                   # the "chocolate chips"
        x, y = rng.random() * S, rng.random() * S
        r = rng.uniform(.008, .020) * S
        d.ellipse([x, y, x + r, y + r], fill=DCU[4] if rng.random() < .4 else (238, 232, 214))
    d.rectangle([0, .34 * S, S, .66 * S], fill=VEST)      # vest band
    d.rectangle([0, .34 * S, S, .36 * S], fill=(64, 68, 44))
    d.rectangle([0, .64 * S, S, .66 * S], fill=(64, 68, 44))
    for u in (.10, .34, .58, .82):                        # pouches
        d.rectangle([u * S, .42 * S, (u + .13) * S, .58 * S], fill=(76, 80, 50))
        d.rectangle([u * S, .42 * S, (u + .13) * S, .45 * S], fill=(112, 116, 78))
    return im.resize((N, N), Image.BILINEAR)


def ci4(im, path, tlut=None):
    """Quantise against a fixed 16-entry TLUT -- what actually goes in TMEM."""
    if tlut:
        pal = Image.new("P", (1, 1))
        flat = [c for rgb in tlut for c in rgb] + [0] * (768 - 3 * len(tlut))
        pal.putpalette(flat)
        q = im.quantize(palette=pal, dither=Image.Dither.NONE).convert("RGB")
    else:
        q = im.quantize(colors=16, method=Image.MEDIANCUT).convert("RGB")
    q.save(path)
    w, h = q.size
    return q, w * h // 2          # bytes as CI4


# ---------------------------------------------------------------- mob model
def build_mob(headgear="helmet", bulk=1.0, pack=None, seed=1):
    m = Mesh()
    W = bulk
    rng = np.random.default_rng(seed + 900)
    Wp = melt_warp(sag=rng.uniform(.34, .48), side=rng.uniform(.50, .80),
                   lump=rng.uniform(.095, .145), seed=seed)
    hy, R = .768, .108

    # --- skull: the melt stays in geometry, the face is a planar decal on the front
    seg, rings = 10, 5
    V, F = blob(0, hy, 0, R * 1.02, R * 1.10, R * .99, seg, rings, warp=Wp)
    U = []                                    # matching points on the *unwarped* sphere
    for j in range(1, rings + 1):
        phi = np.pi * j / (rings + 1)
        for i in range(seg):
            th = 2 * np.pi * i / seg
            U.append((np.sin(phi) * np.cos(th), np.cos(phi), np.sin(phi) * np.sin(th)))
    U += [(0., 1., 0.), (0., -1., 0.)]
    UV = [(.5 + ux * .48, .50 - uy * .46) for (ux, uy, uz) in U]
    tex = [0 if all(U[k][2] > .02 for k in f) else -1 for f in F]
    m.add(V, F, SKIN, "head", uv=UV, tex=tex)
    # ears, different heights, no texture needed
    m.add(*blob(-.104, hy + .028, .002, .016, .032, .024, 5, 1), SKIN_D, "ear")
    m.add(*blob(.100, hy - .036, .000, .017, .026, .022, 5, 1), SKIN_D, "ear")

    # --- body: camo + vest come from the texture
    for grp, args in (("torso", (0, .485, 0, .164 * W, .178, .114 * W, 8, 3)),
                      ("armL", (-.176 * W, .486, -.004, .046 * W, .104, .050, 6, 3)),
                      ("armR", (.176 * W, .486, -.004, .046 * W, .104, .050, 6, 3)),
                      ("legL", (-.074 * W, .215, 0, .058 * W, .152, .062, 6, 2)),
                      ("legR", (.074 * W, .215, 0, .058 * W, .152, .062, 6, 2))):
        warp = (lambda x, y, z: (x * (1 - .22 * y), y, z * (1 - .18 * y))) if grp == "torso" \
            else (lambda x, y, z: (x * (1 + .60 * y * y), y, z * (1 + .60 * y * y)))
        V, F, UV = blob_uv(*args, warp=warp)
        if grp != "torso":                       # limbs sample camo only
            UV = [(u, .04 + v * .26) for (u, v) in UV]
        m.add(V, F, DCU[0], grp, uv=UV, tex=1)
    for sx in (-1, 1):
        m.add(*blob(sx * .078 * W, .044, .028, .064 * W, .046, .104, 6, 2,
                    warp=lambda x, y, z: (x * (1 + .5 * y * y), y, z * (1 + .4 * y * y))),
              (154, 132, 98), "boots")

    # --- headgear, flat-shaded
    if headgear == "helmet":
        m.add(*blob(0, hy + .080, -.006, .132, .082, .138, 8, 2), (154, 138, 100), "helmet")
        m.add(*blob(0, hy + .066, .004, .138, .022, .146, 8, 1), (120, 106, 74), "helmet")
    elif headgear == "boonie":
        m.add(*blob(0, hy + .076, -.006, .108, .052, .112, 8, 2), (172, 154, 110), "hat")
        m.add(*blob(0, hy + .058, -.006, .186, .012, .190, 8, 1), (142, 126, 90), "hat")
    elif headgear == "beret":
        m.add(*blob(0, hy + .078, -.012, .116, .040, .116, 8, 2,
                    warp=lambda x, y, z: (x + .30 * (y + 1) * .5, y, z)), (40, 38, 48), "hat")
    elif headgear == "bandana":
        m.add(*blob(0, hy + .062, -.006, .112, .034, .116, 8, 1), (150, 58, 44), "hat")
    if pack == "canister":
        m.add(*cyl(0, .310, -.126, .070, .350, seg=7), (216, 190, 44), "canister")
    elif pack == "radio":
        m.add(*blob(0, .470, -.126, .076, .108, .038, 6, 2), (96, 100, 64), "radio")
    return m


if __name__ == "__main__":
    face, fb = ci4(paint_face(1), f"{OUT}/tex_cousin_face.png", TLUT)
    body, bb = ci4(paint_body(), f"{OUT}/tex_cousin_body.png")
    TEX = [np.asarray(face, dtype=np.float64) / 255.0,
           np.asarray(body, dtype=np.float64) / 255.0]

    mob = build_mob("helmet", 1.0, "radio", 1)
    mob.write_obj(f"{OUT}/mob_cousin.obj", "cousin", mtllib="mob_cousin.mtl",
                  texnames=["face", "uniform"])
    with open(f"{OUT}/mob_cousin.mtl", "w") as fh:
        fh.write("newmtl face\nmap_Kd tex_cousin_face.png\n\n"
                 "newmtl uniform\nmap_Kd tex_cousin_body.png\n")

    print(f"mob: {mob.tri_count()} tris / {mob.vert_count()} verts")
    print(f"face tex {face.size} CI4 = {fb} B    body tex {body.size} CI4 = {bb} B")

    armed = Mesh().merge(mob).merge(m16(), offset=(.185, .360, .105), rot_y=-.30)
    render(armed, W=760, H=640, cam=(1.10, .70, 2.00), target=(0, .48, .04), fov=33,
           textures=TEX).save(f"{OUT}/mob_hero.png")

    head = render(mob, W=430, H=430, cam=(.20, .820, 1.28), target=(0, .756, .00),
                  fov=25, textures=TEX)
    tiles = [label(head, "TEXTURED FACE", "0 tris spent on features")]
    for a in (0.0, 1.05, 2.10, 3.14, 4.19, 5.24):
        tiles.append(render(mob, W=430, H=430, cam=(np.sin(a) * 2.1, .80, np.cos(a) * 2.1),
                            target=(0, .50, 0), fov=36, textures=TEX))
    sheet(tiles[:6], 3).save(f"{OUT}/mob_sheet.png")

    fr = turntable(mob, frames=24, radius=2.05, height=.76, target=(0, .50, 0),
                   W=340, H=430, fov=35, textures=TEX)
    fr[0].save(f"{OUT}/mob_turnaround.gif", save_all=True, append_images=fr[1:],
               duration=90, loop=0, optimize=True)
