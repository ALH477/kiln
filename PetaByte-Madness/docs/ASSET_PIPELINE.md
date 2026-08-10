<!-- SPDX-License-Identifier: MPL-2.0 -->
# The asset pipeline

How PetaByte Madness' art gets from the drop into a ROM, and the things about
this particular drop that will waste your time if you rediscover them.

Everything here is reproducible: `nix build .#model-<name>`.

## Two paths, and which one a model takes

| path | for | who uses it |
|---|---|---|
| `mkModel` | glTF that is already close to what `gltf_to_t3d` wants | the four demons (`imp`, `hellhound`, `gargoyle`, `overlord`) |
| `mkBlenderModel` | anything needing geometry work first — a rig, a decimate, colours, a split | the centaur, the island, the palms, the LOACH, the guards, the drone |

`mkModel` is a straight conversion. `mkBlenderModel` runs a script from
`tools/blender/` and then the same conversion. Nothing else differs.

```
                       assets/…                tools/blender/
                          │                          │
   mkModel ───────────────┤                          │
                          ▼                          ▼
                    gltf_to_t3d  ◄──── f3d_inject ◄─ blender --background
                          │
                          ▼
              filesystem/models/<name>.t3dm  ──►  rom:/models/<name>.t3dm
```

## The models

| model | source | tris | notes |
|---|---|---|---|
| `centaur` | `machine_centaur_gen.py` | 1,217 | 23 bones, **13 animations**. See below. |
| `island` | `island_n64.obj` | 12,468 → **1,869** | decimated 0.15; flyover only |
| `palms` | `n64_florida_keys_palms.gltf` | 862 | split into `palm_00…05`, `shrub_00…08`, `scatter` |
| `loach` | `loach.obj` | 536 | vertex colours baked in; renders with lighting **off** |
| `guard_*` | `mob_*.obj` | 352–408 | cousin, uncle, whistler, auntie |
| `drone` | `n64_drone_enemy.obj` | 288 | |
| demons | `*.glb` | 440–600 | static only — animations do not survive, see below |

## The centaur, and why it needed two conversions

The centaur arrived as `reference/machine_centaur.h`: F3DEX2 display lists,
23 bones, 13 animations. This engine does not consume display lists.

The first instinct — take the characters' `.glb` files through `mkModel` — is
what the demons do, and it **silently loses every animation**:

```
Channel target not found: hip, skipping channel...
Channel target not found: tail_0, skipping channel...
```

Those are *node* animations on an unskinned hierarchy, and `gltf_to_t3d` wants
a skin. The mesh converts fine, so nothing fails; you just get a statue.

The centaur is one bone per limb across 23 bones — which is **one bone per
vertex**, exactly the constraint `tools/blender/m64lib.py` documents:

> the importer allows ONE bone per vertex and at most three bones per triangle

So an armature is not a workaround here, it is the character's native shape.
Two hops, each with a self-test:

```
machine_centaur_gen.py          (Python: bones, pivots, 13 anims, mesh)
  │  tools/mc_rig_export.py --verify
  ▼
assets/rig/machine_centaur.json (Blender-space, one interchange file)
  │  tools/blender/centaur.py --selftest
  ▼
centaur.gltf ──► centaur.t3dm + centaur.0..12.sdata
```

Result: **0 dropped channels, 23 bones, 13 animation sidecars, 49 KB total**
(23 KB mesh + 26 KB animations). Play them with `m64_skel`.

**This is also the fix for the demons.** Same pipeline, when someone gets to it.

### Why the conversions are verified rather than argued

Both hops change coordinate systems, and a wrong axis or Euler order produces
a model that builds cleanly and is simply wrong — the failure mode
`docs/VEIL_DESIGN.md` §9 records as *"the hellhound's skull was pointing
backwards out of its own neck."*

- `mc_rig_export.py --verify` poses all 23 bones at every keyframe under both
  conventions and asserts they agree. It is checked to **fire in both
  directions**: it rejects the `(x,z,y)` reflection, an unremapped Euler, and a
  swapped Euler sign.
- `centaur.py --selftest` then asserts *Blender* agrees with the source rig —
  3,726 bone-poses, worst delta 4.3e-07. That covers what the first check
  cannot see: bone spaces, Euler order, and the space `pose_bone.location` is
  interpreted in.

The parsing code (`objkit.py`, `mc_rig_export.py`) imports no `bpy` and runs
under a bare `python3`, which is the discipline `tools/blender/quake_map.py`
credits with catching two real bugs before Blender ever ran. It earned its keep
again here — see below.

## Three mesh defects in this drop

All three build "fine" and produce wrong or missing geometry. They are
different problems and get deliberately different treatment.

### 1. Exactly-duplicated faces → drop one

The centaur's `gore` material had two faces on the same three vertices with the
**same winding and the same normal**. F3DEX2 draws both and z-fights; Blender's
`validate()` rejects the mesh outright, which discards the whole material.

Dropped in `mc_rig_export.py`, which also saves a wasted triangle.

### 2. Reversed-winding twins → clone the vertices, keep both

`loach.obj`'s `hull` has two pairs like `(333,334,335)` / `(333,335,334)`.
Same vertices, **opposite** winding: a deliberately double-sided surface.

Blender rejects these too, but dropping one would make the surface vanish from
one side under backface culling. So `objkit.split_double_sided` gives the
second face its own copies of the vertices. Both faces, both windings, culling
intact, three vertices each.

> Check the winding before you dedupe. Same winding is a mistake; opposite
> winding is content.

### 3. A generator that is not idempotent → reset before building

`machine_centaur_gen.add_bone` **appends** to a module-global `BONE_ORDER`.
Calling `build_bones()` twice in one process yields **46 bones instead of 23**
— the dict is overwritten but the order list is not. Every bone appears twice
and poses identically, so no pose check can see it.

`mc_rig_export.load_rig()` clears both globals first and asserts the result has
no duplicates. The bug was caught only by the printed count disagreeing with
`machine_centaur.h`'s own header.

### And one that only bites the checker

`m64lib.make_action` stashes each action in its own NLA track, and **every one
of them evaluates unless muted**. Assigning `animation_data.action` on top of
13 live strips poses the rig with all fourteen at once, which shows up as bones
drifting in animations that never key them. The glTF exporter walks actions
individually — that is what the stashing is *for* — so this only affects code
that evaluates the rig by hand, like `centaur.py --selftest`.

## Conventions

**1 Blender unit = 1 metre.** Every model is scaled to its real size from the
drop's own specs, so the flake's `baseScale` stays a single meaningful dial
instead of a per-model fudge factor, and a wrong number reads as "that
submarine is the size of a car" rather than as a silent mismatch between two
models that were never in the same scene until now.

**The LOACH is deliberately oversized.** `docs/LOACH_spec.md` describes a
one-person boat — 2.86 m, *"crew 1, seated, knees up"*. The centaur is a 2.5 m
quadruped and does not fit by any reading, and the intro needs him to climb
out of it, so it is scaled ×3.5 to about 9.5 m. Chosen over modelling a variant
hull: it costs one number, and the spec's proportions all survive being bigger.

**Vegetation is split, not placed.** The palms glTF is one 862-triangle mesh
containing every plant. Paying that per placement to get one palm is not a
trade worth making, so it is split into connected components and each real
plant becomes a named object. The game places them with
`t3d_model_get_object("palm_00")` plus a matrix each. The ~280 one- and
two-triangle grass blades are merged into a single `scatter` object — invisible
individually at N64 resolution, not worth 280 draw calls, but kept.

## Adding a model

For anything OBJ- or glTF-sourced, add an entry to `MODELS` in
`tools/blender/pm_props.py` and one line to `flake.nix`:

```nix
model-<name> = pmProp "<name>";
```

The entry names the source (relative to `assets/`), a scale to metres, an
optional decimate ratio, where colours come from (`vertex`, `mtl`, or a
procedural function), and how to split into objects (`material`, `group`, or
`loose`).

Sources arrive as `--assets <dir>` — a directory, not a file, because
`loach.obj` resolves `loach.mtl` as a sibling and a Nix store path for a single
file has no siblings.

## Not yet on this path

- **`dank_lab`** and **`patrick_horner`** — both generators emit `.obj`
  (`dank_lab_gen.py`, `patrick_horner_gen.py`), so both are one `MODELS` entry
  away. Horner's generator exposes `POSTURE["slump"]` as a 0..1 dial its own
  header calls *"a usable in-between for a cutscene"*.
- **The demons' animations** — see above.
- **The centaur's CI4 textures.** `mc_face_tlut`, `mc_plate_tlut` and
  `mc_gore_tlut` are byte-identical 16-entry palettes, which is already the
  veil's TLUT format — the natural first real customer for
  `pm_veil_bind_palette`.
- **Island tiling + LOD.** The decimated mesh is for the flyover. Walking on it
  needs `m64_tile` + `m64_lod` + `m64_twopass`; `examples/openworld-demo` is
  the worked example.
