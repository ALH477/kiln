# SPDX-License-Identifier: MPL-2.0
"""keyart.py — the box art: four goblins, their names, and the logo.

    blender --background --factory-startup -noaudio \
        --python tools/blender/keyart.py -- --out renders/keyart.png

── What this is ───────────────────────────────────────────────────────────
A render, not a ROM asset. Nothing here ships on the cartridge. It exists so
the cast can be looked at together — which is the only way to tell whether
four characters actually read as four characters rather than as one goblin in
four colours.

It builds every goblin from goblin.py, puts each in its own "Pose" action,
and lays them out under a logo. Because it drives the real builders, it is
also a regression check with teeth: if a character's geometry or its pose
breaks, the poster breaks visibly rather than a number moving in a test.

── Why the text is geometry ───────────────────────────────────────────────
Everything in this repo renders through Workbench with `color_type='VERTEX'`,
because that is the closest preview to the N64's flat COLOR_0 shading. Text
drawn any other way would need a second renderer and would not match. So the
title and the name plates are converted to meshes and pushed through
m64lib.make_mesh — the same function the goblins use — and come out shaded by
the same rules.

── The style ──────────────────────────────────────────────────────────────
Goofy retro, which here means specific things and not "distressed textures":

  * Every letter is its own object with its own tilt and its own vertical
    offset, hashed from its index. The wobble is the same idea the in-game
    title uses (gg_screens.c's draw_wobble_title) — the poster and the ROM
    are crooked in the same language.
  * Chunky extrusion and a hard black outline, because that is what reads at
    a distance and what N64-era box art actually did.
  * A sunburst behind the cast: flat wedges in two alternating purples. No
    gradient, no glow — the console could not have done either.
"""

import math
import sys
import importlib.util
from pathlib import Path

import bpy

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))

import m64lib as m  # noqa: E402

# ── palette ────────────────────────────────────────────────────────────────
BG_DEEP = m.srgb(24, 14, 46)
RAY_A = m.srgb(46, 26, 84)
RAY_B = m.srgb(36, 20, 68)
TITLE_FACE = m.srgb(64, 255, 148)
TITLE_SIDE = m.srgb(0, 132, 74)
SUB_FACE = m.srgb(255, 206, 92)
PLATE_EDGE = m.srgb(18, 10, 32)

# Seat colours, matching game/src/gg_screens.c's gg_player_tint exactly — the
# poster must not invent a fifth palette.
TINT = [
    m.srgb(0, 245, 120),
    m.srgb(255, 90, 190),
    m.srgb(255, 190, 60),
    m.srgb(90, 200, 255),
]

CAST = ["dank", "sparky", "moss", "glimmer"]
LABEL = ["DANK", "SPARKY", "MOSS", "GLIMMER"]


def jitter(seed):
    """The same hash m64_widget uses, so the poster's crookedness and the
    game's come from one rule."""
    h = (seed * 2654435761) & 0xFFFFFFFF
    h ^= h >> 15
    h = (h * 2246822519) & 0xFFFFFFFF
    h ^= h >> 13
    return ((h >> 8) / 8388608.0) - 1.0


# ── text as geometry ───────────────────────────────────────────────────────

def _text_to_mesh(body, size, extrude, bevel=0.0):
    """Blender text object -> (verts, faces), then thrown away.

    Converted text is n-gons and is not welded; that is fine, it never leaves
    this render. make_mesh's validate() is the only gate it has to pass.
    """
    bpy.ops.object.text_add()
    ob = bpy.context.object
    ob.data.body = body
    ob.data.size = size
    ob.data.extrude = extrude
    ob.data.bevel_depth = bevel
    ob.data.align_x = 'CENTER'
    ob.data.align_y = 'CENTER'
    bpy.ops.object.convert(target='MESH')
    me = ob.data
    verts = [tuple(v.co) for v in me.vertices]
    faces = [tuple(p.vertices) for p in me.polygons]
    bpy.data.objects.remove(ob, do_unlink=True)
    return verts, faces


def letter(name, ch, size, extrude, face, side, at, tilt_deg, roll_deg=0.0):
    """One extruded character, coloured face-vs-side by depth.

    The two-tone is what stops chunky text reading as a flat sticker: the
    front faces take `face`, everything the extrusion created takes `side`.
    Depth is the discriminator because after the rotations below, "front" is
    no longer an axis — but it is still the maximum -Y.
    """
    verts, faces = _text_to_mesh(ch, size, extrude)
    if not verts:
        return None
    verts = m.rotated(verts, (1, 0, 0), 90)          # stand it up, facing -Y
    if roll_deg:
        verts = m.rotated(verts, (0, 1, 0), roll_deg)
    verts = m.rotated(verts, (0, 0, 1), tilt_deg)
    verts = m.translated(verts, *at)

    front = min(v[1] for v in verts)
    cols = [face if v[1] < front + extrude * 0.6 else side for v in verts]
    return m.make_mesh(name, verts, faces, "ArtMat", colors=cols)


def wobble_word(prefix, word, size, extrude, face, side, origin,
                spacing, amp, tilt, seed=0):
    """A word laid out per letter, each with its own tilt and bob."""
    n = len(word)
    total = (n - 1) * spacing
    for i, ch in enumerate(word):
        if ch == " ":
            continue
        dx = origin[0] - total * 0.5 + i * spacing
        dy = origin[1] + jitter(seed + i * 7 + 1) * amp * 0.35
        dz = origin[2] + jitter(seed + i * 7 + 3) * amp
        letter(f"{prefix}{i}", ch, size, extrude, face, side,
               (dx, dy, dz),
               tilt_deg=jitter(seed + i * 7 + 5) * tilt,
               roll_deg=jitter(seed + i * 7 + 9) * tilt * 0.4)


# ── backdrop ───────────────────────────────────────────────────────────────

def sunburst(cx, cz, y, rays=16, radius=26.0):
    """Flat alternating wedges. Two colours, hard edges — a console-era
    backdrop, not a gradient."""
    for i in range(rays):
        a0 = (i / rays) * math.tau
        a1 = ((i + 0.5) / rays) * math.tau
        verts = [(cx, y, cz),
                 (cx + math.cos(a0) * radius, y, cz + math.sin(a0) * radius),
                 (cx + math.cos(a1) * radius, y, cz + math.sin(a1) * radius)]
        m.make_mesh(f"Ray{i}", verts, [(0, 1, 2)], "ArtMat",
                    colors=[RAY_A if i % 2 else RAY_B] * 3)


def plate(name, cx, cz, w, h, y, fill, tilt_deg):
    """A crooked name plate — a quad with a chunky dark border behind it."""
    def quad(hw, hh, colour, depth, suffix):
        v = [(-hw, 0, -hh), (hw, 0, -hh), (hw, 0, hh), (-hw, 0, hh)]
        v = m.rotated(v, (0, 1, 0), tilt_deg)
        v = m.translated(v, cx, y + depth, cz)
        m.make_mesh(f"{name}{suffix}", v, [(0, 1, 2, 3)], "ArtMat",
                    colors=[colour] * 4)
    quad(w * 0.5 + 0.14, h * 0.5 + 0.14, PLATE_EDGE, 0.02, "Edge")
    quad(w * 0.5, h * 0.5, fill, 0.0, "Fill")


# ── the cast ───────────────────────────────────────────────────────────────

def load_goblin(character):
    """Run goblin.py for its scene contribution, without export or reset."""
    sys.argv = ["blender", "--", "--model", character, "--out", "/dev/null"]
    spec = importlib.util.spec_from_file_location(f"gob_{character}",
                                                  ROOT / "goblin.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    arm = None
    for o in bpy.context.scene.objects:
        if o.type == 'ARMATURE' and o.name.startswith("GoblinRig"):
            arm = o                       # the most recent one
    return arm


def main():
    out = m.arg("--out", "keyart.png")
    frame = int(m.arg("--frame", "12"))

    m.reset_scene()

    # goblin.py calls reset_scene() and export_gltf() in its main(); neuter
    # both so four characters can share one scene.
    real_reset = m.reset_scene
    m.reset_scene = lambda: None
    m.export_gltf = lambda path, animated=False: None
    m.report = lambda max_tris=None: None

    spacing = 3.30
    scale = 1.34
    for i, character in enumerate(CAST):
        before = set(bpy.context.scene.objects)
        arm = load_goblin(character)
        if arm is None:
            raise SystemExit(f"keyart: {character} produced no armature")

        # Hold the character's signature pose.
        if arm.animation_data:
            for track in arm.animation_data.nla_tracks:
                track.mute = (track.name != "Pose")

        # Place: a shallow arc so the outer two sit back, and a yaw turning
        # each of them slightly inward. A flat row of four reads as a police
        # line-up; an arc reads as a group.
        x = (i - 1.5) * spacing
        depth = abs(i - 1.5) * 0.55
        for o in bpy.context.scene.objects:
            if o not in before and o.parent is None:
                o.location = (x, depth, 0.0)
                o.scale = (scale, scale, scale)
                # 180 to face the camera, then a few degrees inward so the
                # outer two turn toward the middle rather than standing
                # square. A row of four squared-up characters is a line-up.
                # Only a few degrees: past about 4 the outer two start
                # showing more shoulder than face.
                o.rotation_euler = (0.0, 0.0,
                                    math.radians(180.0 - x * 3.0))

    bpy.context.scene.frame_set(frame)
    bpy.context.view_layer.update()
    m.reset_scene = real_reset

    # ── backdrop, logo, plates ─────────────────────────────────────────
    # Sunburst convergence pushed ABOVE the frame. Wherever it lands inside
    # the picture it is the busiest pixel in the image and it wins — behind
    # the cast it competes with the faces, behind the logo it shows through
    # the counters of the letters. Off the top, all that is left in frame is
    # clean diverging rays, which is the part that was wanted.
    sunburst(0.0, 12.6, 11.0, rays=26, radius=42.0)

    # A dark strip for the cast to stand on. Without it four goblins with
    # nothing under their boots read as floating, which no amount of shadow
    # intensity fixes when the backdrop is also flat.
    ground = [(-9.0, -1.6, -0.02), (9.0, -1.6, -0.02),
              (9.0, 3.2, -0.02), (-9.0, 3.2, -0.02)]
    m.make_mesh("Ground", ground, [(0, 1, 2, 3)], "ArtMat",
                colors=[m.srgb(16, 9, 30)] * 4)

    wobble_word("T", "GANJA", 1.46, 0.32, TITLE_FACE, TITLE_SIDE,
                (0.0, -1.4, 6.30), spacing=1.28, amp=0.22, tilt=7.5, seed=11)
    wobble_word("G", "GOBLIN", 1.46, 0.32, TITLE_FACE, TITLE_SIDE,
                (0.0, -1.4, 4.78), spacing=1.28, amp=0.22, tilt=7.5, seed=53)
    wobble_word("S", "KART KAOS", 0.46, 0.12, SUB_FACE, PLATE_EDGE,
                (0.0, -1.7, 3.52), spacing=0.42, amp=0.08, tilt=5.0, seed=91)

    for i, label in enumerate(LABEL):
        x = (i - 1.5) * spacing
        depth = abs(i - 1.5) * 0.55
        tilt = jitter(i * 13 + 2) * 7.0
        plate(f"Plate{i}", x * 0.94, -0.55, 2.46, 0.68,
              depth - 1.15, TINT[i], tilt)
        wobble_word(f"N{i}_", label, 0.32, 0.08, PLATE_EDGE, PLATE_EDGE,
                    (x * 0.94, depth - 1.32, -0.55), spacing=0.285, amp=0.038,
                    tilt=4.0, seed=i * 31 + 5)

    # ── render ─────────────────────────────────────────────────────────
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.display.shading.light = 'STUDIO'
    scene.display.shading.color_type = 'VERTEX'
    scene.display.shading.show_shadows = False
    scene.display.shading.show_cavity = True
    if hasattr(scene.display.shading, "studiolight_intensity"):
        scene.display.shading.studiolight_intensity = 1.45
    scene.display.shading.show_object_outline = True
    scene.display.shading.object_outline_color = (0.03, 0.02, 0.06)
    scene.display.render_aa = '32'
    scene.render.resolution_x = 1600
    scene.render.resolution_y = 1200
    scene.world = bpy.data.worlds.new("W")
    scene.world.color = (BG_DEEP[0], BG_DEEP[1], BG_DEEP[2])

    cam_data = bpy.data.cameras.new("Cam")
    cam = bpy.data.objects.new("Cam", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    cam_data.lens = 50
    cam.location = (0.0, -17.6, 1.95)

    target = bpy.data.objects.new("T", None)
    target.location = (0.0, 0.0, 2.72)
    scene.collection.objects.link(target)
    con = cam.constraints.new('TRACK_TO')
    con.target = target
    con.track_axis = 'TRACK_NEGATIVE_Z'
    con.up_axis = 'UP_Y'
    bpy.context.view_layer.update()

    scene.render.filepath = out
    scene.render.image_settings.file_format = 'PNG'
    bpy.ops.render.render(write_still=True)
    print("wrote", out)


main()
