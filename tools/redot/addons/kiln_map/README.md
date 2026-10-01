<!-- SPDX-License-Identifier: MIT -->
# Kiln Map — a Redot addon

Author Kiln levels in the Redot (or Godot) editor. The dock shows what the
level costs against what the engine will actually take, and exports the scene
to the `.map` the ROM loads.

## Install

The addon lives in this repo so it is versioned with the format it targets.
Link it into your game's Redot project:

```bash
mkdir -p <your project>/addons
ln -s <kiln>/tools/redot/addons/kiln_map <your project>/addons/kiln_map
```

Then Project → Project Settings → Plugins → enable **Kiln Map**, and set the
**Kiln repo** path in the dock.

If your game has its own classnames, point `KILN_LEVEL_VOCAB_OVERLAY` at its
overlay before launching the editor, so they appear in the palette and stop
being reported as unknown:

```bash
KILN_LEVEL_VOCAB_OVERLAY=~/Documents/PetaByte-Madness/tools/schema/pm_vocab.json redot
```

## What you author

Plain Redot nodes. No script attached to anything, so the scene opens in a
stock editor and this addon is a convenience rather than a requirement.

| node | becomes |
|---|---|
| `CSGBox3D` | a brush. `size` is its full extent, centred on the node. |
| `MeshInstance3D` with a `BoxMesh` | a brush, if you prefer meshes. |
| `CollisionShape3D` with a `ConvexPolygonShape3D` | a brush of any convex shape. |
| any node with `kiln_classname` metadata | a spawn instead. |

| metadata | means |
|---|---|
| `kiln_texture` | that brush's surface name (`DECK`, `WATER`, `STONE`, …) |
| `kiln_classname` | makes the node a spawn |
| `kiln_epair_<key>` | one epair on that spawn |
| `kiln_worldspawn_<key>` | a worldspawn epair (put it on the scene root) |

**Units.** Author at Redot's native 1 unit = 1 metre. The exporter multiplies
by 64, because the engine runs at 64 units to the metre. Set the editor's grid
snap to **0.125 m** and every coordinate lands on a whole engine unit.

**Facing.** A spawn faces along its own **+Z** axis, and the angle the engine
stores is simply the node's Y rotation in degrees. (Godot's "forward is −Z" is
a convention about where a *camera* looks, not a property of the coordinate
system, and it does not apply here.)

## The one trap worth knowing

`kiln_clip` collides **every** brush as its bounding box — `FigBrush` is
`mins`/`maxs` and nothing else. So a brush that is not axis-aligned **draws its
true shape and blocks a box**. A rotated buttress or an angled wall is fine; a
ramp you meant to walk up is not. The dock warns on every such brush, and
`./dev map-validate` lists them.

Keep anything walkable square.

## The dock is not the gate

The counters are exact, and they are deliberately cheap: the dock counts nodes
and measures extents, it does not run the brush CSG. So it cannot tell you that
a convex brush will exceed `MAX_BRUSH_PLANES`, or that a face will exceed
`MAX_FACE_VERTS`.

**Check** and **Export .map** run the real converter
(`tools/mapmaker/tscn_map.py`), which runs the real solver and refuses to write
a level that does not fit. That is the authority; read what it prints.

From a shell, the same thing:

```bash
./dev map-from-tscn level.tscn out.map    # export
./dev map-to-tscn   level.map  out.tscn   # open an existing level
./dev map-validate  out.map               # the full report
./dev map-render    out.map out.png       # see it, with no ROM and no emulator
```

## Why there is no .map code in here

Not one line of this addon knows the `.map` format — no winding table, no plane
maths, and not one of the engine's limits as a GDScript constant. The numbers
are read from `tools/schema/level_vocab.json` at runtime and the file is
written by `tscn_map.py`.

`tools/schema/level_vocab.py`'s docstring records why. These facts once lived
in six hand-maintained copies, and they had drifted — in a tree where an
inside-out brush loads on console, collides correctly, and draws *nothing*.
PetaByte-Madness' `assets/pm_lab.map` shipped exactly that way: all 15 brushes
wound inward, zero faces drawn, for the life of the file. A GDScript emitter
here would be the seventh copy, in the one language no check in this repo runs.
