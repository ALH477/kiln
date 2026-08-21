---
name: n64-animation
description: Rig, skin and animate characters for the Kiln engine (libdragon + Tiny3D on N64). Use when building or fixing an armature, skinned mesh, animation clip or blend; when animations vanish or a limb points the wrong way after conversion; or when working with tools/blender/{goblin,centaur,horner,anim_io}.py, assets/rig/*.json, tools/poser/, gltf_to_t3d's skinning, or kiln_skel.
---

# Animating for the Kiln engine

Two hard limits set everything else, and both are properties of the importer and
the hardware rather than choices:

- **One bone per vertex. Rigid skinning only.** `gltf_to_t3d` reads only the
  first `JOINTS_0` channel per vertex (`tools/gltf_importer/src/parser.cpp` in
  the Tiny3D source), and at most three bones may touch a triangle. There is no
  4-bone weighted blend on console. Design silhouettes that survive rigid
  joints — a shoulder is a socket, not a smooth deformation.
- **`kiln_skel` has exactly two blend slots**, one primary skeleton plus one
  pose-only clone mixed by a live scalar (`kiln_skel_set_blend`). That is an
  idle↔walk locomotion blend and nothing more: no N-way blend tree, no
  upper/lower-body masks, no partial-bone masking.

`kiln_skel_play` does **not** reset bones the new clip does not touch — call
`t3d_skeleton_reset` first, or a limb keeps the last clip's pose.

## The failure mode you are actually guarding against

Every animation defect this project has had built cleanly, validated as glTF, and
was simply wrong:

| defect | how it looked |
|---|---|
| `(x,y,z) → (x,z,y)` used as a rotation | it is a **reflection** — flips handedness, inverts every normal. The hellhound's skull pointed backwards out of its own neck. Forward is `(x, z, -y)`, back is `(x, -z, y)`. |
| an arm rotated about Z by `-k·sx` | swung it *into* the torso. A left arm at negative X needs `+k·sx`. All three bipeds had both arms buried inside the silhouette. |
| tail-chain base rotations positive-X | lifted the tails and exactly cancelled the droop joint they hang from. They read as broom handles. |
| a mesh exported around the FEET, placed by the HIP | Horner floated 96 cm off the floor with his head through the ceiling for an entire cutscene. Nothing failed; the *only* symptom was cameras aimed at him photographing empty room, which reads as a framing bug and gets "fixed" as one. `kiln_transform_push` also rotates about the model origin, so a feet-origin body that pitches to lie down is felled rather than laid flat. Rebase in the EXPORTER, publish the height, gate it (`pm-rigs`). |
| a `.glb` taken through `mkModel` | `Channel target not found: hip, skipping channel…` — **every animation silently lost**. Those are node animations on an unskinned hierarchy and `gltf_to_t3d` wants a skin. The mesh converts fine, so nothing fails; you get a statue. All four PetaByte Madness demons are still statues for this reason. |

Nothing in a build catches any of these. **So the conventions are proved, not
argued** — three layers, each checking what the previous one cannot see:

```
generator (Python: bones, pivots, clips)
  │  PetaByte-Madness/tools/mc_rig_export.py --verify
  │      poses all 23 bones at every keyframe under BOTH conventions and
  │      asserts they agree. Checked to reject the (x,z,y) reflection, an
  │      unremapped Euler, and a swapped Euler sign — i.e. verified to fire.
  ▼
assets/rig/<name>.json          one Blender-space interchange file
  │  tools/blender/centaur.py --selftest
  │      asserts BLENDER agrees with the source rig — 3,726 bone-poses,
  │      worst delta 4.3e-07. This covers what the first check cannot:
  │      bone spaces, Euler order, and the space pose_bone.location is in.
  ▼
<name>.gltf ──► <name>.t3dm + <name>.0..N.sdata
  │  tools/poser/verify.py
  │      proves the EDITOR's Euler convention against Blender's own export.
```

If you add a rigged character, add its selftest. A rig with no pose check is a
rig whose next edit is a guess.

**The first layer is now gated** (`nix/checks/pm-rigs.nix`, `nix flake check`),
and until it was, none of it ran outside someone remembering to type it. That
check does three things, and the second two matter as much as the proof:

- runs both exporters' `--verify` (9,108 bone-poses across the two rigs);
- **regenerates both rig JSONs and diffs them**. They are generated files
  checked into the tree, exactly like the dimension headers `pm-gen-headers`
  polices. Edit a clip, forget the exporter, and the tree builds, converts with
  zero dropped channels, passes every gate and ships the OLD animation — with
  the new source right there in the diff looking applied. There is no symptom:
  the character animates, just not the way the source says;
- asserts the invariants a diff **cannot** catch, because a consistently
  regenerated file can still be wrong for the game. `sit_down`/`stand_up` are 45
  frames because `pm_intake.c` restates them as `0.75f`, and that constant feeds
  `INTRO_T`, which sets the intake camera's key times, which `pm_cine_lint`
  validates — so re-timing a clip silently walks a whole cutscene's camera.
  `climb_in` must end on its last frame (a one-shot freezes there and the
  MOTOR/IN beats expect that pose). A looping clip must open and close on the
  same pose.

So a clip length is not a free parameter. Check what restates it in C before
changing one.

## Getting the animations out at all

`mkBlenderModel { animated = true; }`. Without it the exporter is called with
skins and animations off and you get the statue again — the same symptom as the
`.glb` row above, from a different cause. `bvh = false` for skinned meshes: a
BVH computed against the rest pose describes where the mesh is not.

Check the conversion log for `Channel target not found`. **Zero dropped channels
is the pass condition**; the centaur converts 23 bones and 13 clips with none.

## Two formats, and only one is worth editing

`tools/blender/anim_io.py` is the interchange, and its whole point is choosing
the sparse form:

- **Baked curves** — every 4 frames, every bone, easing already resolved. This
  is what the `.gltf` contains and what the console plays. Right to play, wrong
  to edit: moving one pose means touching sixteen keys.
- **Source keyframes** — four or five poses per clip, each with an easing name.
  This is what an animator manipulates, and what the JSON holds. Rotations are
  XYZ Euler in **degrees**, in Blender pose-bone space — the same space the
  generators' own tables use, so a round trip is the identity.

`anim_io.py` extracts them by **running the builders with `_bake` and
`make_action` replaced by recorders**, not by parsing the source. The actions are
code (`anim_ride_hit` merges deltas over a base pose; `anim_walk` calls
`_step_pose` twice mirrored), so whatever the functions actually compute is what
gets written and there is no second implementation to drift.

## Easing lives in the keys

`kilnlib.make_action` forces **LINEAR** on every keyframe, because the importer
resamples at 60 Hz and Bezier handles would bake overshoot into the result. So
easing is expressed by *sampling* an eased curve into denser linear keys —
`kilnlib.ease(t, mode)` with `in` / `out` / `inout` / `over`, then `_bake(...,
step=4)`.

`step` is a real trade and `tools/poser/verify.py` measures both halves of it:
`goblin.py` picked `step=4` by file size, and the verifier reports the worst
mid-step gap between the ideal eased curve and the piecewise-linear one that
ships. Note the verifier splits its two numbers deliberately — a **CONVENTION**
mismatch (compared only at frames `_bake` actually keyed, where the exported
value IS the sampled value) fails the run; **CURVE LOSS** is reported and never
fails, because it is the cost of `step` and not a bug. An earlier version
conflated them and reported 25° errors that were the test being wrong.

**The rig-JSON characters need the same thing, and one of them went without it
for its whole life.** Easing is applied by whatever builds the clips, so for
Horner and the centaur that is their exporter, not `_bake`. The centaur's has
always sampled — its walk keys every 6 frames, nine per bone. Horner's did not:
every clip was two to five poses with nothing between them, so every motion he
had crossed its full range at a constant speed and stopped dead. That is the
exact "programmer animation" `kilnlib.ease`'s own docstring warns about, sitting
in the game's most finished sequence.

`ph_anim_clips.py` now carries an easing mode per key — the curve used to
ARRIVE at that pose — and `expand()` samples it into the dense linear keys that
ship, so `ph_rig_export.py` is unchanged and its `--verify` covers every sampled
key for free (306 bone-poses → 1,955). A segment marked `None` stays a straight
line and emits nothing extra, which keeps a deliberate hold a hold.

### A two-pose walk cycle is a broken walk cycle

Contact → contact with nothing between is not a cheap walk, it is a
*non-functional* one, and the reason is worth keeping because it is invisible in
a table of angles. Interpolating one contact pose to its own mirror puts **both
legs in exactly the same place** at mid-stride. Measured on Horner's rig through
its own forward kinematics, at frames 10 and 30 the two ankles differed by
`0.00 cm` vertically and `0.00 cm` fore/aft — the legs pass through each other,
and neither foot ever lifts enough to clear the floor. That is the skate.

The fix is a **passing pose** that breaks the symmetry: support leg straight and
carrying the weight, swing leg's knee high enough to clear, toe up, arms at
mid-swing. Same measurement after: `10.4 cm` of lift and `21.0 cm` of stride
separation. Note both halves of that check needed no emulator — `ph_rig.py`'s
`Rig()` and `ph_rig_export._world()` pose the skeleton in plain Python, so
"do the feet clear the floor" is a number you can print, and a screenshot of a
character who is 40 pixels tall is not.

With no root TRANSLATION channel in the rig JSON (rotation only; a walk's travel
belongs to the game's `KilnTransform`), a small **root roll** toward the
unsupported side is the whole of the weight shift. Two degrees. More reads as a
limp.

## The poser

```bash
./dev poser          # three.js editor on :8001
./dev poser-stage    # rebuild the models it loads + re-dump their keyframes
./dev poser-verify   # prove its Euler convention against Blender's export
```

It loads the **same** `.gltf` `nix/blender.nix` already keeps at
`$out/share/gltf/` for debugging, which is what makes it certain you are looking
at what the ROM will contain.

`stage.sh` handles **both kinds of character**: the goblin family (clips in code)
and Horner + the centaur (clips in `assets/rig/*.json`). It staged only the
goblins for a long time, and not by decision — `anim_io.capture()` ran the model
script with `kilnlib` stubbed, which works when the clips are code and returns an
**empty list** for the rig-JSON characters, because their `build_armature` does
real bpy work on the object `make_armature` returns and the stub returns `None`.
Silently. So the two characters PetaByte Madness actually animates had no editor.

`anim_io` now reads the rig JSON directly for those. Two things to know:

- The JSON is **per bone** (`tracks[bone] = [{frame, rot}, …]`, how an exporter
  walks a rig); `_bake` and the poser want **per frame** (`keys = [{frame, ease,
  bones}, …]`, how an animator thinks about a pose). `capture_rig` transposes.
- Their keys are the exporter's own, so easing is already resolved and there is
  no curve to compare — `verify.py` therefore covers the code-clip characters
  only, and the rig-JSON ones are proved by `<script>.py --selftest` instead.
  `stage.sh` splits them for exactly that reason rather than making `verify.py`
  claim more than it checks.

## Two traps in `make_action`

- **Every stashed action evaluates unless muted.** `make_action` puts each action
  in its own NLA track (which is what the exporter's `ACTIONS` mode needs to find
  them), so assigning `animation_data.action` on top of 13 live strips poses the
  rig with all fourteen at once. It shows up as bones drifting in animations that
  never key them. The glTF exporter walks actions individually and is unaffected;
  this only bites code that evaluates the rig by hand, like a `--selftest`.
- **A generator that appends to a module global is not idempotent.**
  `machine_centaur_gen.add_bone` appends to `BONE_ORDER`, so calling
  `build_bones()` twice in one process yields 46 bones instead of 23 — every bone
  twice, posing identically, so no pose check can see it. `load_rig()` clears
  both globals and asserts no duplicates. Caught only by a printed count
  disagreeing with the reference header's own.

## Hand-authored skinned test assets

`tools/gen_skel_gltf.py` is the reference for the minimum a skinned glTF needs:
`skins[].joints` naming a node chain plus `JOINTS_0`/`WEIGHTS_0` mesh attributes.
No Fast64 export and **no `inverseBindMatrices` accessor** — `gltf_to_t3d`
derives inverse bind poses itself from the joint nodes' own TRS hierarchy.

The armature must sit at the origin with no transform, or the importer throws
*"At least one ancestor of armature/skin root bone has significant transforms!"*
(`kilnlib.make_armature` handles this).

## Verifying on console

An animation that plays at the wrong speed, drifts, or T-poses is motion, and a
still frame cannot show motion. Load the **`n64-verify`** skill: take several
frames across the clip and montage them, or `./dev rec` it. For PetaByte Madness,
`nix build .#pm-jump-intake` boots straight into the 14.5-second intake sequence
with no controller needed — that is the loop for iterating Horner's animation.
