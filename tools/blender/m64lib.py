# SPDX-License-Identifier: MPL-2.0
"""m64lib — shared bpy helpers for authoring N64 geometry.

Runs inside `blender --background --python`. Every model script in this
directory imports it; nothing else does.

── What this file is actually for ─────────────────────────────────────────
Blender is a very large tool with a lot of ways to produce geometry that
converts badly for this target. The helpers here are narrow on purpose: they
build meshes from explicit arrays, in linear colour, with one named material,
and then report the numbers that matter on a 93.75 MHz VR4300. Anything not
exposed here (modifiers, subdivision, auto-weights) is left out because its
output is hard to predict at the vertex level, and the vertex level is the
budget.

── Colour ─────────────────────────────────────────────────────────────────
glTF COLOR_0 is linear, and gltf_to_t3d then applies pow(c, 1/2.2) on import
(parser.cpp:251-268) to get the RGBA8 the RDP shades with. So a linear 0.5
does not arrive as 128. Author with `srgb(...)`, which takes the 0-255 value
you want to SEE and pre-inverts that transfer — otherwise every model comes
out looking washed out and it is not obvious why.

── Determinism ────────────────────────────────────────────────────────────
nix/checks/assets.nix builds each asset twice and compares hashes, so nothing
here may depend on iteration order, wall-clock, or a random seed that is not
written down. Objects are created in list order and exported in name order.
"""

import math
import sys

import bpy

# ── colour ─────────────────────────────────────────────────────────────────
GAMMA = 2.2


def srgb(r, g, b, a=255):
    """0-255 sRGB (i.e. what you want on screen) -> linear floats for COLOR_0.

    Inverts the pow(c, 1/2.2) that gltf_to_t3d applies on import, so the byte
    you name here is very close to the byte the RDP ends up shading with.
    """
    return (
        (r / 255.0) ** GAMMA,
        (g / 255.0) ** GAMMA,
        (b / 255.0) ** GAMMA,
        a / 255.0,
    )


def mix(c0, c1, t):
    """Blend two srgb() results in linear space."""
    t = max(0.0, min(1.0, t))
    return tuple(a + (b - a) * t for a, b in zip(c0, c1))


# ── scene ──────────────────────────────────────────────────────────────────
def reset_scene():
    """Empty the file. --factory-startup still gives us the default cube,
    camera and light, and the glTF exporter would happily ship all three."""
    for collection in (bpy.data.objects, bpy.data.meshes, bpy.data.materials,
                       bpy.data.armatures, bpy.data.actions):
        for item in list(collection):
            collection.remove(item)
    bpy.context.scene.frame_start = 0


def make_material(name):
    """A material whose only job is to have a name.

    gltf_to_t3d keys its material table by name and DROPS any primitive whose
    material is missing or unnamed (parser.cpp:150-195) — a silent skip, no
    error, model just comes out empty. The surface properties set here are
    never read: tools/f3d_inject.py writes the real f3d_mat block after export.
    """
    mat = bpy.data.materials.get(name)
    if mat is None:
        mat = bpy.data.materials.new(name)
        mat.use_nodes = False
    return mat


# ── meshes ─────────────────────────────────────────────────────────────────
def make_mesh(name, verts, faces, material, colors=None, uvs=None,
              smooth=False):
    """Build one object from explicit arrays.

    verts    [(x,y,z), ...]                Blender units, Z-up
    faces    [(i,j,k[,l]), ...]            tris or quads, CCW
    material str                           material NAME (see make_material)
    colors   None | one colour | list      per-vertex (len==verts) or
                                           per-face (len==faces), from srgb()
    uvs      None | [(u,v), ...]           per-vertex
    smooth   bool                          smooth vs flat normals
    """
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([tuple(v) for v in verts], [], [tuple(f) for f in faces])
    mesh.update()

    if mesh.validate(verbose=False):
        raise SystemExit(f"m64lib: mesh '{name}' failed validation — most "
                         f"likely a duplicate or out-of-range index")

    for poly in mesh.polygons:
        poly.use_smooth = smooth

    mesh.materials.append(make_material(material))

    if colors is not None:
        # CORNER/FLOAT_COLOR: per-loop and linear. FLOAT_COLOR matters —
        # BYTE_COLOR is sRGB-encoded in Blender and would gamma the values a
        # second time on export.
        attr = mesh.color_attributes.new(name="Color", type='FLOAT_COLOR',
                                         domain='CORNER')
        resolved = _resolve_colors(name, colors, len(verts), len(faces))
        for poly in mesh.polygons:
            for loop_i in poly.loop_indices:
                idx = (poly.index if resolved.per_face
                       else mesh.loops[loop_i].vertex_index)
                attr.data[loop_i].color = resolved.values[idx]
        mesh.color_attributes.active_color_index = 0
        mesh.color_attributes.render_color_index = 0

    if uvs is not None:
        if len(uvs) != len(verts):
            raise SystemExit(f"m64lib: mesh '{name}' has {len(verts)} verts "
                             f"but {len(uvs)} UVs")
        layer = mesh.uv_layers.new(name="UVMap")
        for loop in mesh.loops:
            layer.data[loop.index].uv = uvs[loop.vertex_index]

    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


class _Colors:
    def __init__(self, values, per_face):
        self.values = values
        self.per_face = per_face


def expand_colors(name, colors, verts, faces):
    """Normalise any accepted colour form to one colour per vertex.

    Needed by the skinned path, where a mesh is assembled from many parts
    before any of it reaches Blender, so per-face colours have to be resolved
    up front rather than at loop-fill time.

    Per-face input only round-trips exactly when faces do not share vertices —
    which is true of box(), whose 24 split corners exist precisely so each face
    can hold its own colour. On a shared-vertex mesh the last face touching a
    vertex wins.
    """
    resolved = _resolve_colors(name, colors, len(verts), len(faces))
    if not resolved.per_face:
        return list(resolved.values)

    out = [(1.0, 1.0, 1.0, 1.0)] * len(verts)
    for face_i, face in enumerate(faces):
        for vert_i in face:
            out[vert_i] = resolved.values[face_i]
    return out


def _resolve_colors(name, colors, n_verts, n_faces):
    if isinstance(colors[0], (int, float)):        # one colour for everything
        return _Colors([tuple(colors)] * n_verts, per_face=False)
    if len(colors) == n_verts:
        return _Colors([tuple(c) for c in colors], per_face=False)
    if len(colors) == n_faces:
        return _Colors([tuple(c) for c in colors], per_face=True)
    raise SystemExit(f"m64lib: mesh '{name}' has {n_verts} verts and "
                     f"{n_faces} faces, but {len(colors)} colours — a colour "
                     f"list must match one or the other")


# ── armature + rigid skinning ──────────────────────────────────────────────
def make_armature(name, bones):
    """bones: [(bone_name, parent_name|None, head_xyz, tail_xyz), ...],
    parents before children.

    The armature object is left at the origin with no transform on purpose.
    gltf_to_t3d throws "At least one ancestor of armature/skin root bone has
    significant transforms!" (parser.cpp:118) otherwise — move the geometry,
    never the armature.
    """
    arm = bpy.data.armatures.new(name)
    obj = bpy.data.objects.new(name, arm)
    bpy.context.scene.collection.objects.link(obj)

    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode='EDIT')
    for bone_name, parent, head, tail in bones:
        bone = arm.edit_bones.new(bone_name)
        bone.head = head
        bone.tail = tail
        if parent is not None:
            if parent not in arm.edit_bones:
                raise SystemExit(f"m64lib: bone '{bone_name}' names parent "
                                 f"'{parent}' before it is defined")
            bone.parent = arm.edit_bones[parent]
    bpy.ops.object.mode_set(mode='OBJECT')

    for pbone in obj.pose.bones:
        pbone.rotation_mode = 'XYZ'
    return obj


def make_skinned_mesh(name, parts, armature, material):
    """One mesh, rigidly bound: every vertex belongs to exactly one bone.

    parts: [(bone_name, verts, faces, colors), ...]

    Rigid rather than smooth-weighted because the importer allows ONE bone per
    vertex and at most three bones per triangle (Tiny3D README). Smooth weights
    would be silently truncated to whichever bone happened to be first. Building
    from separate rigid segments makes that constraint structural instead of a
    thing to remember — and for a chunky low-poly character the articulated-
    puppet look it produces is the intent anyway.
    """
    verts, faces, colors = [], [], []
    groups = {}
    order = []
    for bone_name, part_verts, part_faces, part_colors in parts:
        base = len(verts)
        verts.extend(part_verts)
        faces.extend(tuple(base + i for i in f) for f in part_faces)
        colors.extend(expand_colors(f"{name}:{bone_name}", part_colors,
                                    part_verts, part_faces))
        # Accumulate rather than assign: a bone usually owns several pieces
        # (a head also carries its eyes and teeth), and asking Blender for a
        # second vertex group of the same name silently gets you "head.001",
        # whose weights no bone will ever read.
        if bone_name not in groups:
            groups[bone_name] = []
            order.append(bone_name)
        groups[bone_name].extend(range(base, len(verts)))

    obj = make_mesh(name, verts, faces, material, colors=colors, smooth=False)

    for bone_name in order:
        group = obj.vertex_groups.new(name=bone_name)
        group.add(groups[bone_name], 1.0, 'REPLACE')

    obj.parent = armature
    modifier = obj.modifiers.new(name="Armature", type='ARMATURE')
    modifier.object = armature
    return obj


# ── actions ────────────────────────────────────────────────────────────────
def _fcurves(action):
    """Every F-curve in an action, across both action layouts.

    Blender 4.4 introduced "slotted" actions and removed Action.fcurves; the
    curves moved down into layers -> strips -> channelbags. Supporting both
    keeps this working either side of that change, and a nixpkgs Blender bump
    is not something to discover through an AttributeError mid-build.
    """
    if hasattr(action, "fcurves"):
        return list(action.fcurves)
    curves = []
    for layer in action.layers:
        for strip in layer.strips:
            for bag in getattr(strip, "channelbags", []):
                curves.extend(bag.fcurves)
    return curves


def make_action(armature, name, channels, length):
    """Build one action and stash it in its own NLA track.

    channels: {bone_name: [(frame, {'rot': (rx,ry,rz) degrees,
                                    'loc': (x,y,z),
                                    'scale': (x,y,z)}), ...]}

    Stashing matters: the glTF exporter's ACTIONS mode finds actions through
    the object's NLA tracks, so an action left only in bpy.data.actions can
    quietly fail to export and you get a model with a skeleton and no
    animation.

    Every action is keyed at frame 0 and at `length` so its duration is
    explicit; the importer resamples at a fixed 60 Hz (main.cpp) regardless.
    """
    if armature.animation_data is None:
        armature.animation_data_create()

    action = bpy.data.actions.new(name)
    action.use_fake_user = True
    armature.animation_data.action = action

    for bone_name, keys in channels.items():
        if bone_name not in armature.pose.bones:
            raise SystemExit(f"m64lib: action '{name}' keys bone "
                             f"'{bone_name}', which the armature does not have")
        pbone = armature.pose.bones[bone_name]
        for frame, values in keys:
            if 'rot' in values:
                pbone.rotation_euler = [math.radians(a) for a in values['rot']]
                pbone.keyframe_insert(data_path="rotation_euler", frame=frame)
            if 'loc' in values:
                pbone.location = values['loc']
                pbone.keyframe_insert(data_path="location", frame=frame)
            if 'scale' in values:
                pbone.scale = values['scale']
                pbone.keyframe_insert(data_path="scale", frame=frame)

    # Linear everywhere: the importer resamples to 60 Hz anyway, and Blender's
    # default Bezier easing would bake in overshoot we never asked for — most
    # visibly as limbs that swing past their keyed extent.
    for fcurve in _fcurves(action):
        for kp in fcurve.keyframe_points:
            kp.interpolation = 'LINEAR'

    armature.animation_data.action = None
    track = armature.animation_data.nla_tracks.new()
    track.name = name
    track.strips.new(name, 0, action)

    bpy.context.scene.frame_end = max(bpy.context.scene.frame_end, length)
    return action


# ── export ─────────────────────────────────────────────────────────────────
def _supported(kwargs):
    """Keep only the options this Blender's glTF exporter actually has.

    The exporter's option names drift between releases (export_vertex_color
    and export_animation_mode are both relatively recent). Filtering against
    the operator's own RNA means a nixpkgs Blender bump degrades to "that
    option went back to its default" rather than a hard crash mid-build.
    """
    props = bpy.ops.export_scene.gltf.get_rna_type().properties.keys()
    kept = {k: v for k, v in kwargs.items() if k in props}
    dropped = sorted(set(kwargs) - set(kept))
    if dropped:
        print(f"  [GLTF] note: this Blender has no {', '.join(dropped)} "
              f"— using its defaults for those")
    return kept


def export_gltf(path, animated=False):
    """Write <path>.gltf + <path>.bin.

    GLTF_SEPARATE, not GLTF_EMBEDDED: the embedded variant is deprecated in
    current Blender, and gltf_to_t3d reads a plain .gltf with a sidecar .bin
    without complaint. GLB is also fine for the importer but would mean
    rewriting a binary chunk to inject f3d_mat, which is pointless work.
    """
    bpy.ops.export_scene.gltf(**_supported(dict(
        filepath=str(path),
        export_format='GLTF_SEPARATE',
        use_selection=False,
        export_apply=True,           # bake modifiers; not the armature one
        export_yup=True,             # Blender Z-up -> glTF/Tiny3D Y-up
        export_normals=True,
        export_texcoords=True,
        export_tangents=False,
        export_materials='EXPORT',
        export_image_format='NONE',  # textures come from gen_textures+mksprite
        export_cameras=False,
        export_lights=False,
        export_extras=False,         # f3d_inject writes the extras we want
        export_vertex_color='ACTIVE',
        export_all_vertex_colors=False,
        export_skins=animated,
        export_animations=animated,
        export_animation_mode='ACTIONS',
        export_bake_animation=False,
        export_optimize_animation_size=False,
        export_optimize_animation_keep_anim_armature=True,
        export_anim_single_armature=True,
        export_current_frame=False,
        export_frame_range=False,
    )))


def report(max_tris=None):
    """Print the numbers that decide whether this ships, and fail loudly rather
    than late if the budget is blown.

    The 70-vertex note is informational: gltf_to_t3d splits any object past
    MAX_VERTEX_COUNT into chunks by itself (structs.h:290). What it cannot do
    is tell you the total was never affordable in the first place.
    """
    total_v = total_t = 0
    for obj in sorted(bpy.context.scene.objects, key=lambda o: o.name):
        if obj.type != 'MESH':
            continue
        mesh = obj.data
        tris = sum(len(p.vertices) - 2 for p in mesh.polygons)
        total_v += len(mesh.vertices)
        total_t += tris
        note = "  (will be chunked)" if len(mesh.vertices) > 70 else ""
        print(f"  [MESH] {obj.name:<22} {len(mesh.vertices):>4} verts "
              f"{tris:>4} tris{note}")

    print(f"  [MESH] {'TOTAL':<22} {total_v:>4} verts {total_t:>4} tris")

    if max_tris is not None and total_t > max_tris:
        print(f"m64lib: budget exceeded — {total_t} tris against a ceiling of "
              f"{max_tris}. Raise the ceiling deliberately or simplify the "
              f"geometry; do not let it drift.", file=sys.stderr)
        raise SystemExit(1)


def arg(name, default=None):
    """Read `--name value` from the args after Blender's own `--` separator.

    Blender owns everything before the `--`; anything after it is ours.
    """
    argv = sys.argv
    rest = argv[argv.index("--") + 1:] if "--" in argv else []
    if name in rest:
        return rest[rest.index(name) + 1]
    if default is None:
        raise SystemExit(f"m64lib: missing required argument {name}")
    return default


# ── primitive builders ─────────────────────────────────────────────────────
# Explicit array construction rather than bpy.ops.mesh.primitive_*_add: the
# operators produce topology that varies with Blender's version and with the
# operator defaults, and the vertex count is the budget here.

def box(cx, cy, cz, sx, sy, sz):
    """Axis-aligned box centred at (cx,cy,cz). Returns (verts, quads).

    24 verts, not 8: the corners are split per face so each face can carry its
    own flat normal and its own colour. Sharing 8 corners would average the
    normals and give a lit cube a soft, wrong look."""
    hx, hy, hz = sx / 2, sy / 2, sz / 2
    corners = [(cx - hx, cy - hy, cz - hz), (cx + hx, cy - hy, cz - hz),
               (cx + hx, cy + hy, cz - hz), (cx - hx, cy + hy, cz - hz),
               (cx - hx, cy - hy, cz + hz), (cx + hx, cy - hy, cz + hz),
               (cx + hx, cy + hy, cz + hz), (cx - hx, cy + hy, cz + hz)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
             (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    verts, faces = [], []
    for quad in quads:
        base = len(verts)
        verts.extend(corners[i] for i in quad)
        faces.append((base, base + 1, base + 2, base + 3))
    return verts, faces


def uv_sphere(radius, segments, rings, cz=0.0):
    """Shared-vertex UV sphere with poles, plus per-vertex UVs.

    Vertices are shared so normals interpolate smoothly — the opposite choice
    from box(), and deliberately so: this is the model that tests smooth
    normals and the UV seam."""
    verts, uvs, faces = [], [], []
    for ring in range(rings + 1):
        v = ring / rings
        theta = v * math.pi
        for seg in range(segments + 1):
            u = seg / segments
            phi = u * 2 * math.pi
            verts.append((radius * math.sin(theta) * math.cos(phi),
                          radius * math.sin(theta) * math.sin(phi),
                          cz + radius * math.cos(theta)))
            uvs.append((u, v))
    row = segments + 1
    for ring in range(rings):
        for seg in range(segments):
            a = ring * row + seg
            b = a + row
            if ring == 0:
                faces.append((a, b + 1, b))
            elif ring == rings - 1:
                faces.append((a, a + 1, b))
            else:
                faces.append((a, a + 1, b + 1, b))
    return verts, faces, uvs


def cylinder(radius, height, segments, cz=0.0, top_radius=None):
    """Tube plus two caps. top_radius=0 gives a cone.

    The side and the caps do NOT share vertices: a cylinder wants smooth
    normals around the side and a hard edge at the rim, and one shared ring
    cannot be both."""
    top_radius = radius if top_radius is None else top_radius
    verts, faces = [], []

    side_base = len(verts)
    for seg in range(segments + 1):
        phi = seg / segments * 2 * math.pi
        c, s = math.cos(phi), math.sin(phi)
        verts.append((radius * c, radius * s, cz))
        verts.append((top_radius * c, top_radius * s, cz + height))
    for seg in range(segments):
        a = side_base + seg * 2
        faces.append((a, a + 2, a + 3, a + 1))

    for z, radius_at, flip in ((cz, radius, True), (cz + height, top_radius, False)):
        if radius_at == 0.0:
            continue
        base = len(verts)
        verts.append((0.0, 0.0, z))
        for seg in range(segments):
            phi = seg / segments * 2 * math.pi
            verts.append((radius_at * math.cos(phi), radius_at * math.sin(phi), z))
        for seg in range(segments):
            i0 = base + 1 + seg
            i1 = base + 1 + (seg + 1) % segments
            faces.append((base, i1, i0) if flip else (base, i0, i1))

    return verts, faces


def torus(major, minor, major_segments, minor_segments):
    """Fully closed, shared vertices, smooth. The point of this shape is that
    it occludes itself, which is what makes it a Z-buffer test."""
    verts, faces = [], []
    for i in range(major_segments):
        u = i / major_segments * 2 * math.pi
        for j in range(minor_segments):
            v = j / minor_segments * 2 * math.pi
            r = major + minor * math.cos(v)
            verts.append((r * math.cos(u), r * math.sin(u), minor * math.sin(v)))
    for i in range(major_segments):
        for j in range(minor_segments):
            a = i * minor_segments + j
            b = ((i + 1) % major_segments) * minor_segments + j
            a2 = i * minor_segments + (j + 1) % minor_segments
            b2 = ((i + 1) % major_segments) * minor_segments + (j + 1) % minor_segments
            faces.append((a, b, b2, a2))
    return verts, faces


def loft(sections, cap_start=True, cap_end=True):
    """Skin a sequence of equal-length rings into a tube. Returns (verts, faces).

    This is the primitive the fixed shapes above cannot express: anything whose
    cross-section changes along its length — a tapering fuselage, a canopy, a
    nacelle. Every other builder here is a special case of it.

    sections   [[(x,y,z), ...], ...]   >=2 rings, all the same length

    ── Winding ────────────────────────────────────────────────────────────
    Faces come out CCW-outward, i.e. correctly front-facing under the RDP's
    back-face culling, when BOTH of these hold:

      * each ring lists its points in increasing angle about the sweep axis
        (for a sweep along +Y, that is increasing atan2(z, x));
      * consecutive rings advance in the positive sweep direction.

    Reverse either one and the whole object is inside-out — which on hardware
    reads as "the model is invisible from outside and solid from within", not
    as an error. There is no way to detect the intent here, so the rule is
    stated rather than enforced.

    ── Shared vertices ────────────────────────────────────────────────────
    Rings are NOT split between sections, so a lofted shape can be shaded
    smooth. Flat shading still works (it uses face normals), so sharing costs
    a faceted model nothing — unlike box(), where the split exists so each
    face can carry its own COLOR_0.

    Caps are a fan from a ring's own centroid, which is only correct for a
    convex ring. Concave cross-sections want cap_start/cap_end False and a
    hand-built cap.
    """
    if len(sections) < 2:
        raise SystemExit("m64lib: loft needs at least two sections")
    n = len(sections[0])
    if n < 3:
        raise SystemExit("m64lib: loft sections need at least three points")
    for i, ring in enumerate(sections):
        if len(ring) != n:
            raise SystemExit(f"m64lib: loft section {i} has {len(ring)} points, "
                             f"but section 0 has {n} — sections must match")

    verts, faces = [], []
    for ring in sections:
        verts.extend(tuple(p) for p in ring)

    for s in range(len(sections) - 1):
        a, b = s * n, (s + 1) * n
        for i in range(n):
            j = (i + 1) % n
            faces.append((a + i, b + i, b + j, a + j))

    # The two fans wind opposite ways, because the two caps face opposite ways
    # along the sweep. Which way round that is, is not obvious from the ring
    # order and was wrong here first time — the check is in
    # tools/blender/test_prims.py, which is why it did not reach Blender.
    for cap, base, first in ((cap_start, 0, True),
                             (cap_end, (len(sections) - 1) * n, False)):
        if not cap:
            continue
        ring = verts[base:base + n]
        centre = len(verts)
        verts.append(tuple(sum(c) / n for c in zip(*ring)))
        for i in range(n):
            j = (i + 1) % n
            faces.append((centre, base + i, base + j) if first
                         else (centre, base + j, base + i))

    return verts, faces


def slab(points):
    """Extrude a polygon into a closed solid. Returns (verts, faces).

    points   [(x, y, z_centre, half_thickness), ...]

    Each point carries its own centre and half-thickness, so one call covers a
    flat plate, a tapered aerofoil and a twisted fin — the alternative is a
    Blender solidify modifier, whose output is exactly the kind of thing this
    module exists to avoid guessing at.

    The polygon must be CCW in the XY projection (thickness runs along Z) and
    convex, for the same fan-cap reason as loft().
    """
    n = len(points)
    if n < 3:
        raise SystemExit("m64lib: slab needs at least three points")

    verts = [(x, y, zc - h) for x, y, zc, h in points]        # 0..n-1  bottom
    verts += [(x, y, zc + h) for x, y, zc, h in points]       # n..2n-1 top

    faces = [tuple(range(n, 2 * n)),                          # top, +Z
             tuple(reversed(range(n)))]                       # bottom, -Z
    for i in range(n):
        j = (i + 1) % n
        faces.append((i, j, n + j, n + i))
    return verts, faces


def mirror_x(points):
    """Mirror a slab()/loft() point list across X=0, reversing it so the
    winding survives. Negating x alone flips every face inward."""
    return [(-p[0],) + tuple(p[1:]) for p in reversed(points)]
