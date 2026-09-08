#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""meshy_bake.py — a generated .glb down to something a vertex-colour
pipeline can draw.

    blender --background --factory-startup -noaudio --python meshy_bake.py \
        -- --src build/meshy/<n>/model.glb --albedo build/meshy/<n>/albedo.png \
           --out assets/<n>.glb --name kiln --tris 420 --height 2.016

An AUTHORING-TIME step, run by hand like the rest of the Meshy lane
(tools/meshy.py). Its output is committed; nothing in `nix build` runs it.

Two of these four stages exist because the obvious version is broken, and
both were found by rendering the result and looking at it.

── 1. WELD BEFORE YOU DECIMATE ────────────────────────────────────────────
A Meshy mesh arrives with its UV seams split: the kiln was 2,858 vertices
for 2,087 triangles. Collapse-decimating that does not simplify it, it
SHATTERS it — a collapse across a seam moves one copy of the vertex and
leaves the other behind, so the surface tears into loose shards with holes
between them. The first attempt at 420 triangles came out as a spray of
spikes, and it builds and converts perfectly happily.

remove_doubles first takes the kiln to 1,038 vertices and the same 2,087
triangles, and decimation from there is clean at every budget tried
(2087 -> 899 -> 419 all read identically at 320x240).

── 2. HUE FROM THE PALETTE, VALUE FROM THE ALBEDO ─────────────────────────
The natural thing is to sample the albedo per vertex and use it directly.
That gives a flat pinkish beige: a 2048x2048 photo-real texture sampled at a
few hundred points loses every brick course and converges on its own mean.
It looked worse than the procedural kiln it was meant to improve on, whose
header already says why — "brick courses are discrete steps, not a
gradient".

So `--band` supplies the hue as discrete courses chosen by HEIGHT, and the
albedo contributes only its luminance, which is where the course lines, the
soot and the doorway's darkness actually live. The result keeps the authored
art direction and gains the generated detail.

This matters for the boot splash specifically because
nix/checks/kiln-splash.nix converts with `--ignore-materials`: the splash
draws SHADE only, so a texture would be discarded and vertex colours are the
only channel there is.
"""

import os
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kilnlib as m  # noqa: E402

# Default courses: the three terracotta values kiln_logo.py already tuned for
# this splash, base -> body -> neck, darkening toward the top "as if the neck
# sees the most soot".
DEFAULT_BAND = "146,66,42 128,54,34 94,42,28"

# Where the height bands meet, as a fraction of total height.
BAND_STOPS = (0.10, 0.72)

# The albedo's luminance is remapped into this multiple of the course colour.
# Above 1.0 at the top end so a mortar highlight still lifts off the brick;
# the bottom end keeps a recess black rather than merely dim.
VALUE_LO, VALUE_HI = 0.45, 1.15


def srgb_to_linear(c):
    """kilnlib.srgb takes 0-255 ints; this takes a float and does the same
    transfer, because the sampled albedo is not an authored constant."""
    c /= 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def load(src):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=src)
    objs = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    if not objs:
        raise SystemExit("meshy_bake: %s has no mesh" % src)
    bpy.ops.object.select_all(action='DESELECT')
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    if len(objs) > 1:
        bpy.ops.object.join()
    return bpy.context.view_layer.objects.active


def weld(obj):
    """See stage 1 in the module docstring. This is not an optimisation."""
    before = len(obj.data.vertices)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.mesh.remove_doubles(threshold=1e-4)
    bpy.ops.object.mode_set(mode='OBJECT')
    print("  welded %d -> %d verts (%d tris)"
          % (before, len(obj.data.vertices), len(obj.data.polygons)))


def decimate(obj, target):
    n = len(obj.data.polygons)
    if not target or target >= n:
        print("  no decimation (%d tris, target %s)" % (n, target or "none"))
        return
    mod = obj.modifiers.new("Decimate", 'DECIMATE')
    mod.ratio = target / n
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.modifier_apply(modifier=mod.name)
    print("  decimated %d -> %d tris" % (n, len(obj.data.polygons)))


def sample_luma(obj, albedo):
    """Mean albedo luminance per vertex, 0..1. {} if there is no albedo."""
    if not albedo:
        return {}
    from PIL import Image
    me = obj.data
    if not me.uv_layers.active:
        raise SystemExit("meshy_bake: no UVs, so the albedo cannot be sampled")
    im = Image.open(albedo).convert("RGB")
    w, h = im.size
    px = im.load()
    uvs = me.uv_layers.active.data
    acc = {}
    for poly in me.polygons:
        for li in poly.loop_indices:
            vi = me.loops[li].vertex_index
            u, v = uvs[li].uv
            x = min(w - 1, max(0, int(u * w)))
            y = min(h - 1, max(0, int((1.0 - v) * h)))
            r, g, b = px[x, y]
            a = acc.setdefault(vi, [0.0, 0])
            a[0] += 0.2126 * r + 0.7152 * g + 0.0722 * b
            a[1] += 1
    return {vi: (a[0] / a[1]) / 255.0 for vi, a in acc.items()}


def colour(obj, bands, luma):
    """Discrete courses by height, modulated by the albedo's luminance."""
    me = obj.data
    zs = [v.co[2] for v in me.vertices]
    z0, z1 = min(zs), max(zs)
    span_z = max(z1 - z0, 1e-6)
    vals = list(luma.values()) or [0.5]
    lo, hi = min(vals), max(vals)
    span_v = max(hi - lo, 1e-6)

    # Drop anything already there and keep exactly ONE layer, active for
    # both render and edit. The first version left two: the export came out
    # with COLOR_0 and COLOR_1, and every consumer that reads "the" vertex
    # colour then picks one by luck. The kiln rendered pure white that way —
    # a real layer of terracotta sitting unused behind a default.
    while me.color_attributes:
        me.color_attributes.remove(me.color_attributes[0])
    layer = me.color_attributes.new(name="Color", type='FLOAT_COLOR',
                                    domain='POINT')
    me.color_attributes.active_color = layer
    me.color_attributes.default_color_name = layer.name
    me.color_attributes.active_color_name = layer.name
    for vi, vert in enumerate(me.vertices):
        t = (vert.co[2] - z0) / span_z
        band = bands[0] if t < BAND_STOPS[0] else (
            bands[1] if t < BAND_STOPS[1] else bands[-1])
        k = VALUE_LO
        if vi in luma:
            k = VALUE_LO + (VALUE_HI - VALUE_LO) * ((luma[vi] - lo) / span_v)
        layer.data[vi].color = tuple(
            srgb_to_linear(min(255.0, c * k)) for c in band) + (1.0,)
    print("  coloured %d verts: %d courses, albedo luma %.2f-%.2f"
          % (len(me.vertices), len(bands), lo, hi))


def place(obj, height):
    """Base on Z=0, centred in XY, scaled to `height`.

    A model's world size is fixed by its source and its authoring scale and is
    NOT something a call site gets to guess at — so it is normalised once,
    here, to the height the consumer's camera is framed for.
    """
    me = obj.data
    mn = [min(v.co[i] for v in me.vertices) for i in range(3)]
    mx = [max(v.co[i] for v in me.vertices) for i in range(3)]
    s = height / max(mx[2] - mn[2], 1e-6) if height else 1.0
    for v in me.vertices:
        v.co = ((v.co[0] - (mn[0] + mx[0]) / 2) * s,
                (v.co[1] - (mn[1] + mx[1]) / 2) * s,
                (v.co[2] - mn[2]) * s)
    me.update()
    ext = [max(v.co[i] for v in me.vertices) - min(v.co[i] for v in me.vertices)
           for i in range(3)]
    print("  extent  X %.3f  Y %.3f  Z %.3f" % tuple(ext))
    return ext


def main():
    src = m.arg("--src")
    out = m.arg("--out")
    albedo = m.arg("--albedo", "")
    name = m.arg("--name", "body")
    tris = int(m.arg("--tris", "0"))
    height = float(m.arg("--height", "0"))
    bands = [tuple(int(c) for c in part.split(","))
             for part in m.arg("--band", DEFAULT_BAND).split()]
    if len(bands) < 2:
        raise SystemExit("meshy_bake: --band needs at least two courses")

    print("── %s ──" % name)
    obj = load(src)
    print("  in: %d tris %d verts" % (len(obj.data.polygons),
                                      len(obj.data.vertices)))
    weld(obj)
    decimate(obj, tris)
    colour(obj, bands, sample_luma(obj, albedo))
    place(obj, height)

    obj.name = name
    obj.data.materials.clear()
    # The material name is what gltf_to_t3d keys its table by, and it SKIPS a
    # primitive whose material is missing or unnamed — silently, with a zero
    # exit status. The consumer converts with --ignore-materials, but a bare
    # `.glb` with no material at all is a trap for anything that does not.
    m.make_material(name)
    if obj.data.materials:
        obj.data.materials[0] = bpy.data.materials[name]
    else:
        obj.data.materials.append(bpy.data.materials[name])

    n_col = len(obj.data.color_attributes)
    if n_col != 1:
        raise SystemExit("meshy_bake: %d colour attributes, expected 1. Two "
                         "of them export as COLOR_0 and COLOR_1 and the "
                         "consumer picks one by luck." % n_col)
    # export_vertex_color='ACTIVE' plus export_all_vertex_colors=False, both
    # stated rather than left to default. The defaults write the active layer
    # AND every layer, so one attribute came out as both COLOR_0 and COLOR_1 —
    # the same terracotta twice — and a consumer reading "the" vertex colour
    # picks one by luck. That is what rendered the kiln pure white.
    bpy.ops.export_scene.gltf(filepath=out, export_format='GLB',
                              export_apply=True,
                              export_vertex_color='ACTIVE',
                              export_all_vertex_colors=False)
    print("  -> %s (%d bytes)" % (out, os.path.getsize(out)))


main()
