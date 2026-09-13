---
name: n64-animation
description: Rig, skin and animate characters for the Kiln engine (libdragon + Tiny3D on N64). Use when building or fixing an armature, skinned mesh, animation clip or blend; when animations vanish or a limb points the wrong way after conversion; or when working with tools/blender/{goblin,anim_io}.py, assets/rig/*.json, tools/poser/, gltf_to_t3d's skinning, or kiln_skel.
---

# Animating for the Kiln engine

Two hard limits set everything else, and both are properties of the importer and
the hardware rather than choices:

- **One bone per vertex. Rigid skinning only.** `gltf_to_t3d` reads only the
  first `JOINTS_0` channel per vertex (`tools/gltf_importer/src/parser.cpp` in
  the Tiny3D source), and at most three bones may touch a triangle. There is no
  4-bone weighted blend on console. Design silhouettes that survive rigid
  joints — a shoulder is a socket, not a smooth deformation.
- **`kiln_skel` has three slots and no blend tree.** BASE and BLEND are the
  locomotion pair, mixed by one scalar (`kiln_skel_set_blend`, or
  `kiln_skel_crossfade` into the lighter slot). OVERLAY is blended over the
  result for a bone MASK — `kiln_skel_mask_bone(sk, "torso")` for an attack or
  a wave that leaves the legs running. Resolve bones by NAME: the exporter's
  order is not the script's. Props ride a bone through `kiln_skel_bone_push`
  (bone matrices are MODEL units, x64 baked in); a head turns with
  `kiln_skel_bone_rotate` each frame before `kiln_skel_update`.
- **Clip time is real seconds at 24 fps** — nothing sets Blender's scene rate.
  Read `kiln_skel_length`, never a frame count. A one-shot on the overlay fades
  out before its end, because Tiny3D stops a finished clip without applying
  its last pose.

## Feet: measure the ground speed, then play at travel / ground speed

A walk or run clip moves its planted foot backwards at some speed; the game
moves the character at another. Unless playback rate = travel speed / ground
speed, the feet skate. `tools/blender/gait.py` measures it by forward
kinematics over the shipped glTF (or `--source goblin --rig <gltf>` over
goblin.py's keys, before paying for a Blender run), along with swing clearance
and the lowest contact. Publish the number as `GOBLIN_*_MPS` in the C;
`nix/checks/goblin-gait.nix` holds both the clip and the C to it. While two
locomotion clips blend, give both one CYCLE rate (rate x own length) so they
stay on the same foot, and phase-match the incoming clip on a slot swap.

A rotation-only rig cannot crouch: bending the knees RAISES the feet. Lower the
body by the feet's rise (`kiln_skel_bone_pos` on the foot bones) while grounded.
A roll spins the whole body about a published pivot (`ROLL_PIVOT_M`) through a
nested transform; the clip only holds the tuck.

`kiln_skel_play` does **not** reset bones the new clip does not touch — call
`t3d_skeleton_reset` first, or a limb keeps the last clip's pose.

## The failure mode you are actually guarding against

Every animation defect this project has had built cleanly, validated as glTF,
and was simply wrong:

| defect | how it looked |
|---|---|
| `(x,y,z) → (x,z,y)` used as a rotation | it is a **reflection** — flips handedness, inverts every normal. A model's head pointed backwards out of its own neck. Forward is `(x, z, -y)`, back is `(x, -z, y)`. |
| an arm rotated about Z by `-k·sx` | swung it *into* the torso. A left arm at negative X needs `+k·sx`. Every biped tested had both arms buried inside the silhouette until this was caught. |
| tail-chain base rotations positive-X | lifted the tails and exactly cancelled the droop joint they hang from. They read as broom handles. |
| a mesh exported around the FEET, placed by the HIP | a character floated most of a body-height off the floor with its head through the ceiling, for a whole cutscene. Nothing failed; the *only* symptom was cameras aimed at it photographing empty room, which reads as a framing bug and gets "fixed" as one. `kiln_transform_push` also rotates about the model origin, so a feet-origin body that pitches to lie down is felled rather than laid flat. Rebase in the EXPORTER, publish the height, and gate that a call site still agrees with it. |
| a `.glb` taken through `mkModel` | `Channel target not found: hip, skipping channel…` — **every animation silently lost**. Those are node animations on an unskinned hierarchy and `gltf_to_t3d` wants a skin. The mesh converts fine, so nothing fails; you get a statue. |

Nothing in a build catches any of these. **So the conventions are proved, not
argued** — three layers, each checking what the previous one cannot see:

```
generator (Python: bones, pivots, clips)
  │  a Python --verify step
  │      poses every bone at every keyframe under BOTH conventions and
  │      asserts they agree. A convention proof needs to be checked to
  │      REJECT a reflection, an unremapped Euler, and a swapped Euler
  │      sign before it's trusted to accept anything.
  ▼
assets/rig/<name>.json          one Blender-space interchange file (only
  │                              needed for a character whose clips arrive
  │  a Blender --selftest         as rig JSON rather than as code, see below)
  │      asserts BLENDER agrees with the source rig. This covers what the
  │      first check cannot: bone spaces, Euler order, and the space
  │      pose_bone.location is in.
  ▼
<name>.gltf ──► <name>.t3dm + <name>.0..N.sdata
  │  tools/poser/verify.py
  │      proves the EDITOR's Euler convention against Blender's own export.
```

If you add a rigged character, add its selftest. A rig with no pose check is a
rig whose next edit is a guess. `tools/blender/goblin.py` (the shared
rigged/animated reference — used by `examples/cinematic-demo`) is this
repo's own worked example of the code-clip path; put a new one's pure Python
logic under `nix/checks/blender-tests.nix`'s `test_*.py` glob so it's asserted
on natively rather than only ever looked at.

A convention proof like this earns its keep by catching real regressions, not
just by existing — regenerating a checked-in rig JSON and diffing it against
the tree is part of the same discipline: a generated file that silently goes
stale ships the OLD animation with the NEW source sitting right there in the
diff looking applied, and there is no other symptom.

## Getting the animations out at all

`mkBlenderModel { animated = true; }`. Without it the exporter is called with
skins and animations off and you get the statue again — the same symptom as the
`.glb` row above, from a different cause. `bvh = false` for skinned meshes: a
BVH computed against the rest pose describes where the mesh is not.

Check the conversion log for `Channel target not found`. **Zero dropped
channels is the pass condition.**

## Two formats, and only one is worth editing

`tools/blender/anim_io.py` is the interchange, and its whole point is choosing
the sparse form:

- **Baked curves** — every 4 frames, every bone, easing already resolved. This
  is what the `.gltf` contains and what the console plays. Right to play, wrong
  to edit: moving one pose means touching sixteen keys.
- **Source keyframes** — four or five poses per clip, each with an easing name.
  This is what an animator manipulates. Rotations are XYZ Euler in **degrees**,
  in Blender pose-bone space — the same space a generator's own tables use, so
  a round trip is the identity.

`anim_io.py` extracts them by **running the builders with `_bake` and
`make_action` replaced by recorders**, not by parsing the source. The actions
are code (an idle/walk builder calls its own pose function twice mirrored, a
hit reaction merges deltas over a base pose), so whatever the functions
actually compute is what gets written and there is no second implementation
to drift.

For a character whose clips arrive as rig JSON instead of code (an exporter-
driven pipeline rather than a hand-written Blender script), `anim_io` reads
the JSON directly: it is **per bone** (`tracks[bone] = [{frame, rot}, …]`, how
an exporter walks a rig), while `_bake` and the poser want **per frame**
(`keys = [{frame, ease, bones}, …]`, how an animator thinks about a pose) —
`capture_rig` transposes between the two. Those keys are already the
exporter's own with easing resolved, so there is no curve to compare against;
`verify.py` covers code-clip characters only, and a rig-JSON character is
proved by its own generator's `--selftest` instead.

## Easing lives in the keys

`kilnlib.make_action` forces **LINEAR** on every keyframe, because the importer
resamples at 60 Hz and Bezier handles would bake overshoot into the result. So
easing is expressed by *sampling* an eased curve into denser linear keys —
`kilnlib.ease(t, mode)` with `in` / `out` / `inout` / `over`, then `_bake(...,
step=4)`.

`step` is a real trade and `tools/poser/verify.py` measures both halves of it:
a builder picks its `step` by file size, and the verifier reports the worst
mid-step gap between the ideal eased curve and the piecewise-linear one that
ships. The verifier splits its two numbers deliberately — a **CONVENTION**
mismatch (compared only at frames `_bake` actually keyed, where the exported
value IS the sampled value) fails the run; **CURVE LOSS** is reported and
never fails, because it is the cost of `step` and not a bug. An earlier
version conflated them and reported errors that were the test being wrong.

For a rig-JSON character, easing is applied by whatever builds the clips
(the exporter, not `_bake`), and it is easy to ship one that never sampled at
all — every clip two to five poses with nothing between them, so every motion
crosses its full range at a constant speed and stops dead. That is the exact
"programmer animation" `kilnlib.ease`'s own docstring warns about. The fix is
an easing mode per source key that a transpose step samples into the dense
linear keys that ship, so the exporter itself stays unchanged and its
`--verify` covers every sampled key for free.

### A two-pose walk cycle is a broken walk cycle

Contact → contact with nothing between is not a cheap walk, it is a
*non-functional* one, and the reason is worth keeping because it is invisible
in a table of angles. Interpolating one contact pose to its own mirror puts
**both legs in exactly the same place** at mid-stride — measurable through
plain forward kinematics in Python, no emulator needed: pose the skeleton at
both frames and diff the ankle heights. A cycle with this defect measures
(effectively) zero vertical separation between the two ankles at mid-stride —
the legs pass through each other and neither foot ever lifts enough to clear
the floor. That is the skate.

The fix is a **passing pose** that breaks the symmetry: support leg straight
and carrying the weight, swing leg's knee high enough to clear, toe up, arms
at mid-swing. With no root TRANSLATION channel in the rig (rotation only; a
walk's travel belongs to the game's own transform), a small **root roll**
toward the unsupported side is the whole of the weight shift — a couple of
degrees; more reads as a limp.

## The poser

```bash
./dev poser          # three.js editor on :8001/poser/ (./dev studio saves in place)
./dev poser-stage    # rebuild the models it loads + re-dump their keyframes
./dev poser-verify   # prove its Euler convention against Blender's export
```

It loads the **same** `.gltf` `nix/blender.nix` already keeps at
`$out/share/gltf/` for debugging, which is what makes it certain you are
looking at what the ROM will contain.

`tools/poser/stage.sh` handles both kinds of character (code-clip and
rig-JSON) via `anim_io.capture()`/`capture_rig()` — worth knowing if you add a
rig-JSON character: `anim_io.capture()` runs the model script with `kilnlib`
stubbed, which works when the clips are code, but a rig-JSON character's
`build_armature` does real bpy work on the object `make_armature` returns,
and a stub silently returns `None` for that — an empty-list capture that
looks like "no clips" rather than "no editor support", if `capture_rig`'s
separate rig-JSON path isn't wired up for it.

## Two traps in `make_action`

- **Every stashed action evaluates unless muted.** `make_action` puts each
  action in its own NLA track (which is what the exporter's `ACTIONS` mode
  needs to find them), so assigning `animation_data.action` on top of many
  live strips poses the rig with all of them at once. It shows up as bones
  drifting in animations that never key them. The glTF exporter walks actions
  individually and is unaffected; this only bites code that evaluates the rig
  by hand, like a `--selftest`.
- **A generator that appends to a module global is not idempotent.** A
  `add_bone`-style helper that appends to a module-level bone-order list means
  calling the builder twice in one process yields every bone twice, posing
  identically — so no pose check can see it, only a printed count disagreeing
  with a reference header's own. Clear the global and assert no duplicates in
  whatever loads the rig.

## Hand-authored skinned test assets

`tools/gen_skel_gltf.py` is the reference for the minimum a skinned glTF needs:
`skins[].joints` naming a node chain plus `JOINTS_0`/`WEIGHTS_0` mesh attributes.
No Fast64 export and **no `inverseBindMatrices` accessor** — `gltf_to_t3d`
derives inverse bind poses itself from the joint nodes' own TRS hierarchy.

The armature must sit at the origin with no transform, or the importer throws
*"At least one ancestor of armature/skin root bone has significant transforms!"*
(`kilnlib.make_armature` handles this).

## Verifying on console

An animation that plays at the wrong speed, drifts, or T-poses is motion, and
a still frame cannot show motion. Load the **`n64-verify`** skill: take
several frames across the clip and montage them, or `./dev rec` it. A jump
ROM that boots straight into the animated sequence, with no controller
needed, is the loop for iterating on a character's animation without walking
a menu chain first every time.
