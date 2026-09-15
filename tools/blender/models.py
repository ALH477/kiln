# SPDX-License-Identifier: MIT
"""models.py — the test-geometry set.

    blender --background --factory-startup -noaudio \
        --python tools/blender/models.py -- --model cube --out build/cube

Each model exists to make one specific class of renderer bug visible. That is
the selection criterion: a shape that looks fine whether or not the thing under
test works is not a test. Where a model is textured it is marked (tex) and its
material name is what tools/f3d_inject.py must be told about.

── Axis convention ────────────────────────────────────────────────────────
Blender is Z-up; glTF and therefore Tiny3D are Y-up, and the exporter converts
with export_yup. The mapping is:

    Blender +X  ->  engine +X
    Blender +Z  ->  engine +Y   (up)
    Blender +Y  ->  engine -Z

Geometry here is authored in Blender space. The `axes` model is coloured by
ENGINE axis, not Blender axis, because the engine's is the one you are ever
actually trying to confirm.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import kilnlib as m  # noqa: E402

WHITE = m.srgb(255, 255, 255)


# ── axes ───────────────────────────────────────────────────────────────────
def build_axes():
    """RGB gizmo. The first thing to look at when anything renders wrong: if
    the arrows do not point where the engine says they should, every other
    model in this set is being judged in a mirror.

    Coloured by ENGINE axis. The Blender direction each one is built along is
    the second column, and it is the export_yup conversion in the module
    docstring that makes those two columns differ."""
    shaft, tip, thin = 1.6, 0.45, 0.16

    for label, blender_dir, color in (
        ("X", "+X", m.srgb(255, 76, 106)),   # engine +X  = Blender +X
        ("Y", "+Z", m.srgb(76, 255, 130)),   # engine +Y  = Blender +Z (up)
        ("Z", "-Y", m.srgb(90, 150, 255)),   # engine +Z  = Blender -Y
    ):
        dx, dy, dz = _DIRS[blender_dir]
        verts, faces = m.box(dx * shaft / 2, dy * shaft / 2, dz * shaft / 2,
                             shaft if dx else thin,
                             shaft if dy else thin,
                             shaft if dz else thin)
        m.make_mesh(f"Axis{label}Shaft", verts, faces, "AxisMat", colors=color)

        # A cone, not a smaller box: a box tip reads as a T and says nothing
        # about which end is the far end.
        cone_v, cone_f = m.cylinder(0.28, tip, 6, cz=shaft, top_radius=0.0)
        m.make_mesh(f"Axis{label}Tip", _along(cone_v, blender_dir), cone_f,
                    "AxisMat", colors=color)

    verts, faces = m.box(0, 0, 0, 0.34, 0.34, 0.34)
    m.make_mesh("Origin", verts, faces, "AxisMat", colors=m.srgb(240, 240, 240))


_DIRS = {"+X": (1, 0, 0), "-X": (-1, 0, 0), "+Y": (0, 1, 0),
         "-Y": (0, -1, 0), "+Z": (0, 0, 1), "-Z": (0, 0, -1)}

# Rotations taking geometry built along +Z (which is what m.cylinder does) onto
# each axis. Each is a signed axis permutation, so it stays exact in floating
# point and cannot introduce a scale.
_ROT = {
    "+Z": lambda x, y, z: (x, y, z),
    "-Z": lambda x, y, z: (x, -y, -z),
    "+X": lambda x, y, z: (z, y, -x),
    "-X": lambda x, y, z: (-z, y, x),
    "+Y": lambda x, y, z: (x, z, -y),
    "-Y": lambda x, y, z: (x, -z, y),
}


def _along(verts, direction):
    rot = _ROT[direction]
    return [rot(*v) for v in verts]


# ── cube ───────────────────────────────────────────────────────────────────
def build_cube():
    """Baseline, and directly comparable to the cube examples/engine builds by
    hand in C — same shape, opposite route to the screen."""
    verts, faces = m.box(0, 0, 0, 2.2, 2.2, 2.2)
    face_colors = [
        m.srgb(255, 76, 106), m.srgb(76, 255, 130), m.srgb(255, 217, 76),
        m.srgb(0, 245, 212), m.srgb(139, 92, 246), m.srgb(255, 140, 60),
    ]
    m.make_mesh("Cube", verts, faces, "CubeMat", colors=face_colors)


# ── sphere ─────────────────────────────────────────────────────────────────
def build_sphere():
    """Smooth normals across shared vertices, banded by latitude. Two things
    show up here and nowhere else in the set: normal interpolation (bands
    should gradate, not step), and the importer's 70-vertex chunk split, since
    this is comfortably past it."""
    verts, faces, _uvs = m.uv_sphere(1.3, 12, 8)
    cool, warm = m.srgb(40, 90, 200), m.srgb(255, 190, 60)
    colors = [m.mix(cool, warm, (v[2] + 1.3) / 2.6) for v in verts]
    m.make_mesh("Sphere", verts, faces, "SphereMat", colors=colors, smooth=True)


# ── cylinder / cone ────────────────────────────────────────────────────────
def build_cylinder():
    """Smooth around the side, hard at the rim. Proves flat and smooth normals
    coexist in one mesh — if the exporter merges the rim vertices the silhouette
    goes soft where it should be a crease."""
    verts, faces = m.cylinder(0.9, 2.2, 14, cz=-1.1)
    colors = [m.mix(m.srgb(0, 245, 212), m.srgb(20, 60, 120),
                    (v[2] + 1.1) / 2.2) for v in verts]
    m.make_mesh("Cylinder", verts, faces, "CylinderMat", colors=colors,
                smooth=True)


def build_cone():
    verts, faces = m.cylinder(1.15, 2.4, 14, cz=-1.2, top_radius=0.0)
    colors = [m.mix(m.srgb(255, 140, 60), m.srgb(255, 240, 200),
                    (v[2] + 1.2) / 2.4) for v in verts]
    m.make_mesh("Cone", verts, faces, "ConeMat", colors=colors, smooth=True)


# ── torus ──────────────────────────────────────────────────────────────────
def build_torus():
    """The only shape in the set that occludes itself. A convex model looks
    correct with the Z-buffer disabled; this one does not, so it is what
    actually tests depth and back-face culling."""
    verts, faces = m.torus(1.25, 0.45, 16, 10)
    colors = [m.mix(m.srgb(139, 92, 246), m.srgb(255, 76, 106),
                    (v[2] + 0.45) / 0.9) for v in verts]
    m.make_mesh("Torus", verts, faces, "TorusMat", colors=colors, smooth=True)


# ── stress ─────────────────────────────────────────────────────────────────
def build_stress():
    """Two ribbons straddling MAX_VERTEX_COUNT (70, structs.h:290): one at 68
    verts that fits in a single chunk, one at 72 that cannot.

    A chunk-split regression in the importer or the ucode drops part of a mesh,
    and on any ordinary model that reads as "some triangles look wrong". Here
    it reads as "one of the two ribbons is visibly shorter than the other",
    which is a thing you can actually see in a screenshot."""
    for label, columns, z_off, base in (("Under", 34, 0.7, m.srgb(76, 255, 130)),
                                        ("Over", 36, -0.7, m.srgb(255, 76, 106))):
        verts, faces, colors = [], [], []
        for i in range(columns):
            t = i / (columns - 1)
            x = (t - 0.5) * 4.0
            for row in (0, 1):
                verts.append((x, 0.0, z_off + (row - 0.5) * 0.55))
                colors.append(m.mix(base, m.srgb(255, 255, 255), t))
        for i in range(columns - 1):
            a = i * 2
            faces.append((a, a + 2, a + 3, a + 1))
        name = f"Ribbon{label}"
        assert len(verts) == columns * 2
        m.make_mesh(name, verts, faces, "StressMat", colors=colors)


# ── textured ───────────────────────────────────────────────────────────────
def build_checker():
    """(tex) Two flat quads with UVs running 0..2, one material wrapping and
    one mirroring.

    Flat and axis-aligned on purpose: the UV pixel-coord conversion
    (s = u * texWidth * 32, meshConverter.cpp:120) is only obviously wrong when
    the squares stop being square, and on a curved surface you would not be able
    to tell that from foreshortening."""
    for name, material, y in (("CheckerWrap", "CheckerWrapMat", -1.3),
                              ("CheckerMirror", "CheckerMirrorMat", 1.3)):
        verts = [(-1.2, y, -1.2), (1.2, y, -1.2), (1.2, y, 1.2), (-1.2, y, 1.2)]
        uvs = [(0.0, 0.0), (2.0, 0.0), (2.0, 2.0), (0.0, 2.0)]
        m.make_mesh(name, verts, [(0, 1, 2, 3)], material,
                    colors=WHITE, uvs=uvs)


def build_uvsphere():
    """(tex) The grid texture on a sphere: UV seam behaviour, pole pinching,
    and a second mksprite format (RGBA16 rather than I8) in one model."""
    verts, faces, uvs = m.uv_sphere(1.3, 16, 10)
    m.make_mesh("UVSphere", verts, faces, "GridMat", colors=WHITE, uvs=uvs,
                smooth=True)


# ── animated-surface subjects ──────────────────────────────────────────────
# Two shapes the flat test set cannot stand in for, both built for
# examples/texanim-demo. `checker` is two parallel vertical quads a model-width
# apart — right for judging square texels, useless as ground.
TILEFLOOR_HALF = 2.4        # Blender units; x64 = +-154 world units
TILEFLOOR_CELLS = 8
TILEFLOOR_UV_PER_CELL = 0.5 # 4 texture repeats across the floor


def build_tilefloor():
    """(tex) A subdivided floor whose UVs run past 1, for a scrolling texture.

    The scroll is a tile-translate on the RDP, so what breaks it is the wrap:
    UVs that stop at 1 show a single tile and hide a wrong repeat mode. Eight
    cells rather than one quad because fog and lighting are per vertex — a
    single quad this size fogs as one flat gradient."""
    n = TILEFLOOR_CELLS + 1
    verts, faces, uvs = [], [], []
    for j in range(n):
        for i in range(n):
            verts.append((-TILEFLOOR_HALF + 2 * TILEFLOOR_HALF * i / TILEFLOOR_CELLS,
                          -TILEFLOOR_HALF + 2 * TILEFLOOR_HALF * j / TILEFLOOR_CELLS,
                          0.0))
            uvs.append((i * TILEFLOOR_UV_PER_CELL, j * TILEFLOOR_UV_PER_CELL))
    for j in range(TILEFLOOR_CELLS):
        for i in range(TILEFLOOR_CELLS):
            a = j * n + i
            faces.append((a, a + 1, a + n + 1, a + n))  # CCW from +Z: faces up
    m.make_mesh("TileFloor", verts, faces, "FloorMat",
                colors=m.srgb(214, 226, 244), uvs=uvs, smooth=True)


FLAG_LENGTH = 3.0           # hoist (x = 0) to fly
FLAG_HEIGHT = 1.8
FLAG_COLS = 12
FLAG_ROWS = 6


def build_flag():
    """A banner in the Blender XZ plane, hoist edge on x = 0, bottom on z = 0.

    Built as a grid because it is deformed on the CPU every frame (kiln_deform):
    the wave is a function of each vertex's x, so the columns ARE the
    resolution of the ripple. Faces wind +X then +Z, so the front faces Blender
    -Y, which is engine +Z. Per-face colours split vertices along the band
    edges; the wave reads position, so the seams stay closed."""
    n = FLAG_COLS + 1
    verts, faces, colors = [], [], []
    for j in range(FLAG_ROWS + 1):
        for i in range(n):
            verts.append((FLAG_LENGTH * i / FLAG_COLS, 0.0,
                          FLAG_HEIGHT * j / FLAG_ROWS))
    bands = (m.srgb(139, 92, 246), m.srgb(232, 232, 240), m.srgb(0, 200, 180))
    hoist = m.srgb(242, 160, 60)
    for j in range(FLAG_ROWS):
        for i in range(FLAG_COLS):
            a = j * n + i
            faces.append((a, a + 1, a + n + 1, a + n))
            colors.append(hoist if i == 0 else bands[j * len(bands) // FLAG_ROWS])
    m.make_mesh("Flag", verts, faces, "FlagMat", colors=colors, smooth=True)


MODELS = {
    "axes": (build_axes, 140),
    "cube": (build_cube, 24),
    "sphere": (build_sphere, 200),
    "cylinder": (build_cylinder, 80),
    "cone": (build_cone, 60),
    "torus": (build_torus, 360),
    "stress": (build_stress, 160),
    "checker": (build_checker, 8),
    "uvsphere": (build_uvsphere, 320),
    "tilefloor": (build_tilefloor, 2 * TILEFLOOR_CELLS * TILEFLOOR_CELLS),
    "flag": (build_flag, 2 * FLAG_COLS * FLAG_ROWS),
}


def main():
    name = m.arg("--model")
    out = m.arg("--out")
    if name not in MODELS:
        raise SystemExit(f"models.py: no model '{name}'; "
                         f"have {', '.join(sorted(MODELS))}")
    build, max_tris = MODELS[name]

    m.reset_scene()
    build()
    m.report(max_tris=max_tris)
    m.export_gltf(out)


main()
