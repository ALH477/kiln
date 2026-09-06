---
name: n64-forge
description: Author levels, cinematics and textures ON the console with Forge (.#forge) — the voxel editor whose save button emits Quake .map brushes, and the SD-card loop that makes it work on an ED64 Plus with no USB. Use when building or editing level geometry for this engine, when moving content between the repo and a flashcart (./dev forge-push / forge-pull), when a level saved on hardware does not come back, when working on kiln_voxel / kiln_voxmesh / kiln_store or tools/forge/frg.py, or when deciding whether content should be authored in Blender, the three.js mapmaker, or Forge.
---

> **Before you tune anything visual, know what has and has not been seen.**
> Forge's CI4 atlas used to be uploaded and then discarded: `kiln_voxmesh` puts
> the block type only in the UVs and vertex colour is greyscale
> `DIR_SHADE[dir]`, so under `RDPQ_COMBINER_SHADE` — which `kiln_scene_begin`
> sets every frame — all fifteen block types rendered as the same grey and a
> greedy-merged wall read as one flat slab. `T3D_FLAG_TEXTURED` only makes the
> RSP emit texture coordinates; the combiner decides whether the texel survives.
>
> **Fixed:** `begin_voxel_state` (`Forge/src/forge_geo.c`) now sets
> `RDPQ_COMBINER_TEX_SHADE`, and the atlas bind follows `Z` so the veiled
> preview changes the geometry rather than only the swatches. `kiln-voxmesh`
> still renders both combiners and greps `forge_geo.c` to catch the call going
> away — it sets both itself, so the pixels alone cannot see which one Forge
> picks.
>
> **Still open, and it is the reason PAINT exists:** nothing has been booted.
> Whether the authored ramp still separates once the veil discards hue, and
> what texturing every voxel face costs in fill rate, are judgements about a
> CRT that no host capture settles. Make them on hardware.

# Forge — editing on the machine the content runs on

`nix build .#forge`. Read `n64-verify` first if the question is "what is the ROM
doing"; this file is about *authoring*.

Three authoring tools already existed here and all three run on the host:
`./dev mapmaker` (:8000), `./dev poser` (:8001), `tools/blender/*`. Forge exists
because the judgements that decide whether a level is good on this hardware
cannot be made two hops away — fill rate, whether a palette still separates once
the veil discards hue, whether a corridor reads as a corridor at 320x240.

## 0. Which tool for which job

| want | use |
|---|---|
| a room, corridor, arena — anything made of boxes | **Forge** |
| a character, a prop, a rig, an animation | `tools/blender/*` + `mkBlenderModel` |
| a level laid out with a mouse and a big screen | `./dev mapmaker`, then `forge-push` it to refine on console |
| an existing `.map` adjusted where you can walk it | `./dev forge-push <file.map>` |

Forge authors **axis-aligned volumes**. That is not a limitation bolted on; it is
the whole reason it fits. A greedy-merged run of blocks IS an `KilnBrush`, which
IS what `kiln_map.c` reduces a Quake brush to, which IS what `mapio.js` emits.
Slopes, cylinders and characters belong in Blender.

## 1. On an ED64 Plus, run the probe first

```bash
nix build .#forge-selftest && cp -L result/forge-selftest.z64 /path/to/sd/
```

It reports the cart, the backend, a 64 KB write/read/verify, and PASS or FAIL.
Everything else here assumes it passed, and it takes one boot to find out.

**Why it matters:** the cart has **no USB**. `libdragon/src/usb.c:527` rejects
EverDrive 2.5-class boards outright, so `debugf`, `./dev deploy`, `./dev debug`
and UNFLoader all see nothing. The SD card is the only channel, in both
directions. What makes that fine is that **libcart is already vendored in the
pinned libdragon and names this cart**: `src/libcart/cart.h:7-12` —
`CART_ED` = *"EverDrive-64 V1, V2, V2.5, V3 and ED64+"*.

If it FAILS, nothing is lost: `kiln_store` walks SD → 32 KB save chip (flushed to
the card by holding RESET ~2 s) → read-only `rom:/`. Both fallbacks are slower
and both are already built. `forge-selftest-sram` is the probe for the middle
rung, and it exists because a gate should fire in both directions — the plain
probe must report *no writable backend* under an emulator.

## 2. The loop

```bash
nix build .#forge && cp -L result/forge.z64 "$(./dev forge-card)/"   # once
./dev forge-push assets/oot_test.map          # seed a level onto the card
#   ... boot on the N64, build, press START ...
./dev forge-pull                              # -> assets/ + the validator
```

**The ROM is built once. Levels are data on the card.** Editing costs no
rebuild and no reflash — that is the property the whole design is arranged
around, and it is what makes iteration viable without USB.

`./dev forge-card` prints where the card is, or fails loudly. It refuses to
guess: writing a level into the wrong directory is silent and the ROM will not
find it next boot. Override with `FORGE_CARD=/path`.

On the card: `FORGE/LEVEL.FRG` (the working format), `FORGE/LEVEL.MAP` (the
derived brushes), `FORGE/FORGE.LOG` (a text log — the only place a diagnostic
can go that is not the screen).

**The ROM emits the `.map` as ASCII itself**, in the dialect already pinned
here, so `forge-pull` is a copy plus `./dev map-validate` rather than a bespoke
decoder. It also re-derives the same `.map` on the host and tells you whether
the two agree, because their agreeing is worth noticing and their disagreeing is
worth investigating.

## 3. Six modes, and the controls

`L+R` cycles, in the order you work in: block it out, walk it, texture it, dress
it, light it, shoot it. Every mode has a jump ROM — `nix build .#forge-geo`,
`.#forge-walk`, `.#forge-paint`, `.#forge-ent`, `.#forge-light`, `.#forge-cam` —
because `./dev shot` has no input path at all and `./dev drive`'s chain is
fragile, so a mode reached only by a chord is a mode verified only by hand.
(`.#forge-geo` is new; GEO is Forge's default boot mode, so `.#forge` is the
same ROM, but the set is uniform now.)

**This table is GENERATED from `Forge/src/forge_binds.def`** —
`python3 tools/forge/gen_binds.py`. It used to be written here by hand and was
wrong in five ways, the worst being `Z`+`A` to fill: `forge_geo.c` anchors on Z
**press** and fills on Z **release**, and suppresses `A` for the whole drag, so
the documented button did nothing. `forge_hud.c`'s in-ROM help said the same
thing, which is where it was copied from.

| mode | control | does |
|---|---|---|
| GEO | `A` | put |
|  | `B` | dig |
|  | `Z` | drag-fill (press anchors, release fills) |
|  | `L` | pick the aimed block type |
|  | `dpad-lr` | block type |
|  | `dpad-ud` | rise/fall |
|  | `stick` | fly |
|  | `C-stick` | look |
|  | `R` | sprint |
| WALK | `stick` | walk |
|  | `C-stick` | look |
|  | `R` | run |
| PAINT | `dpad` | texel cursor |
|  | `A` | draw (held) |
|  | `B` | pick colour |
|  | `C-lr` | colour |
|  | `C-ud` | tile |
|  | `R` | flood tile |
|  | `Z` | veiled preview (held) |
| ENT | `A` | place/move |
|  | `B` | remove |
|  | `dpad-lr` | classname |
|  | `dpad-ud` | epair field |
|  | `R` | epair + |
|  | `Z` | epair - |
|  | `stick` | fly |
|  | `C-stick` | look |
| LIGHT | `C-ud` | field |
|  | `dpad` | edit |
|  | `A` | fog on/off |
|  | `R` | clear colour |
| CAM | `A` | insert key |
|  | `B` | delete key |
|  | `Z` | play (held) |
|  | `dpad-lr` | scrub |
|  | `dpad-ud` | key step |
|  | `R` | duration +1s |
|  | `L` | duration -1s |
|  | `stick` | fly (not while playing) |
| every mode | `L+R` | next mode |
|  | `START` | save |

**PAINT and LIGHT deliberately do not move the camera.** The D-pad is a texel
cursor or a light aim, and holding the view still is what makes the judgement
possible — a light judged while the camera moves is a light judged against a
moving target.

**PAINT is on the console because a palette-swap mechanic (`tools/veil_palette.py`,
`assetLib.mkVeilTexture`) can discard hue entirely** — a filter where only
VALUE carries makes a ramp separated by hue stop reading altogether. Whether
16 colours still separate through a TLUT swap at 320x240 on a CRT is not
answerable from a host preview — `Z` flips between a level's two baked
palettes with the geometry still on screen behind the canvas.

**CAM validates before it saves.** `kiln_camlint` — the same module
`./dev cine-lint` runs — checks the table every frame, and a hard failure means
the `.KEY` file is *refused and logged* rather than written. `eye == look` halts
the VR4300 inside `t3d_viewport_attach`, several layers from the table that caused
it; an editor that writes that table has moved the failure two tools away from the
person holding the controller.

CAM also draws **both** curves: dim is the straight chord through the keys,
bright is what `kiln_camkey_sample` actually produces. The gap between them IS the
Catmull-Rom overshoot, invisible as coordinates and obvious as a bulge. The amber
one is the LOOK path, which overshoots the same way and is usually what makes a
camera feel drunk.

Look is on the C-**buttons** as well as the C-stick deliberately:
`kiln_fpscam` reads only the stick and a real N64 controller does not have a
second one. Fly speed scales off the far plane, because one speed is
unusable across both a 512-unit room and an 8192-unit world.

**WALK is the mode that justifies the tool.** It installs the greedy-meshed
boxes as the real clip world and hands the pad to the real `kiln_fpscam` — not an
approximation. A doorway's width is judged by walking through it.

## 4. Read the gauges, not the picture

Same principle as the debug overlay, and for the same reason: four of the five
worst defects in this engine passed `nix build` and `nix flake check` and were
found by a number.

```
FORGE GEO  blk 1  60 fps
chunks 1/24   solid 92
quads 14  arena 0%
clip 0/512
eye -192 256 -192  near 8 far 6000
aim 6 1 7 t1  put 6 1 6  d 450
cart none  store rom  bus -
load ok
```

- **`chunks n/24`** — red at the cap, where further blocks are being *refused*.
  An editor that silently drops an edit is read as a mis-aim.
- **`quads` and `arena %`** — the count is printed *as well as* the percentage
  because greedy merging is effective enough that a whole room is ~26 quads out
  of a 3072-quad arena, so the honest percentage rounds to `0%`. A gauge reading
  0 over plainly-drawn geometry is one nobody believes the next time it reads 0
  for a real reason.
- **`clip n/512`** — red at 0 in WALK, where an empty clip world means falling
  forever. That exact composition (a working camera over no clip world) is what
  made PLAY a black screen with a working HUD for its entire life.
- **`aim`** — in BLOCK coordinates, because that is what a level is authored in.
  `put` is the cell a new block would occupy; its box is **red when that cell is
  off the grid**, which happens constantly and legitimately when you aim at the
  outside of a boundary wall.
- **`cart` / `store` / `bus`** — printed every frame, not behind a key, so any
  screenshot carries the answer to "could this have saved?"
- **`load ok` vs `new ok`** — `new` means nothing was saved yet. Anything else in
  red names the step that refused. The first version reported both as `new ok`,
  which is how a ROM that had never mounted its filesystem looked exactly like a
  clean first boot.

## 5. `kiln_camkey` / `kiln_camlint` — one implementation, four consumers

The keyframe curve and its validator were originally local to PetaByte
Madness. Both were promoted into engine modules once Forge needed the same
curve authored, drawn and validated on-console; PetaByte Madness (now its own
repo) keeps thin shim headers over the engine versions — typedefs and
defines, not a second implementation — so its existing keyframe tables
compile untouched.

- `kiln_camkey` is **header-only** and stays that way. Its own file explains why:
  a `.c` would have to be added to two ROMs' `OBJS` and two native check compile
  lines. `engine/Makefile` grew a `HEADER_ONLY` list for it, because `MODULES`
  drives `$(OBJS)` and a name there with no `.c` fails the archive.
- Include it as `<kiln/kiln_camkey.h>` — the installed-prefix spelling. The native
  checks put `engine/src` (not `engine/src/kiln`) on their include path so the same
  spelling resolves in both places.
- The invariant: the runtime flies this curve, the overlay draws it, the validator
  measures it, and Forge authors it. An author tuning against a curve that merely
  resembles the shipped one is the same defect as a validator measuring one.

## 6. The two reductions, and why they are different

`kiln_voxel` performs both, and confusing them is the mistake to avoid:

- **`kiln_voxel_boxes`** — a *volume* partition into non-overlapping boxes that
  exactly tile the solid set. Collision, `.map` export, WALK mode.
- **`kiln_voxel_quads`** — a *surface* extraction, greedy-merged rectangles over
  faces that have air on the other side. Rendering only.

Drawing the boxes would draw the faces where two boxes meet, and on this
hardware **fill rate is the limit, not triangle count** (the island terrain
budget that held 60 fps is ~830 triangles; a full-screen sea plane is the
expensive part). Interior faces are the purest wasted fill there is.

A 480-block room is 3 boxes and 26 quads.

## 7. Where to put a test

**`kiln_voxel` is host-tested; `kiln_voxmesh` is not, and that seam is
deliberate.** `kiln_voxel` includes only `<stdint.h>` and `<t3d/t3dmath.h>`, so
it compiles against `nix/checks/stub/` and is asserted on in `kiln-logic` in
milliseconds. `kiln_voxmesh` includes `<t3d/t3d.h>` and cannot be — which is
precisely why the vertex packing was split out of the mesher rather than the
other way round.

So: **arithmetic over the block grid goes in `nix/checks/kiln-logic-check.c`.**
Content formats go in `nix/checks/forge-roundtrip.nix` (via
`tools/forge/frg.py selftest`, which is `bpy`-free and runs under a bare
`python3`).

Properties already asserted there, each of which failed at least once:

- boxes cover the solid set **exactly once** (a double-covered block is a brush
  the player sticks inside)
- merged quad **area** equals the exposed-face count, and no quad covers an
  unexposed face (area alone cannot catch one missing plus one spurious)
- a chunk seam is **not** a wall — the mesher fetches neighbours through
  `kiln_voxel_get` on *world* coordinates, and meshing against the chunk's own
  array welds a wall across every seam
- the raycast's place-cell is in **front** of the face (derived from the step
  taken, not from the normal's sign, which is how it comes out backwards)
- a ray fired from **outside** the grid still hits inside it. This one shipped
  broken: the DDA bailed on the first out-of-bounds cell instead of the first
  cell from which the grid is unreachable, and since an editor camera sits
  outside the level looking in, *every* ray missed. The symptom was `aim -` with
  a room filling the screen.
- `.map -> voxels -> .map` is byte-identical, AND importing **conserves** the
  geometry. Idempotency alone is not enough: an import that drops every brush is
  perfectly idempotent, and that is exactly what happened.

## 8. Things that cost time here

- **A `.map` centred on the origin has half its geometry at negative block
  coordinates.** Block indices are unsigned by construction, so `voxelise`
  translates by the map's floored min corner into `world.offset` and the `.FRG`
  carries it back. Without that pass `assets/oot_test.map` imported as **zero
  solid blocks**, silently.
- **Round OUTWARD on import, never to nearest.** `oot_test.map`'s floor is 4
  units thick against a 32-unit block — 0.125 blocks — so rounding both corners
  to the nearest index collapsed it to nothing. mins floors, maxs ceils, and a
  brush is never allowed zero volume. A wall that got thicker is visible and
  reported; a wall that vanished is silent, and this is an import of someone's
  existing level.
- **`sram_detect()` returns 0, not −1, when there is no save chip** — its own doc
  comment in `sram.h` says −1. A `< 0` test cannot fail, so the SRAM backend was
  selected on machines with no chip, after which writes went nowhere and reads
  came back as zeros, which parse as a valid *empty* directory.
- **`assets` in `mkN64Rom` is silently ignored unless the Makefile declares a
  DFS.** Two lines, and without them the ROM is byte-for-byte the assetless
  build. This cost a downstream game its filesystem once, and Forge repeated
  it exactly. `nix/rom.nix` now fails the build and prints the two lines,
  naming the real make target taken from the emitted `.z64` rather than the
  flake attribute.
- **`kiln_clip`'s broadphase must stay OFF** for voxel-derived brushes. The grid
  is 16x16 in XZ with Y ignored and its placement pass `assertf`s at 512
  brush-cell entries, which one floor slab trips. That assert is a hard crash.
- **Forge ROMs are under 1 MiB**, which `nix/checks/rom.nix` NOTEs. Fine for
  emulators and SC64; if an ED64+ menu refuses one, pad it.
- **A `.map` has no camera path**, so a level imported by `frg.py frommap` comes
  in with zero keys and CAM correctly reports `ERR nokeys`. The mode-jump ROMs
  therefore ship NO assets and fall through to `forge_io_seed`'s demo content,
  which has a spawn and a three-key shot. Teaching the importer to invent a camera
  path instead would put content nobody authored into every imported level. Demo
  content has exactly one author.
- **Three demo keys, not two.** Catmull-Rom takes its tangent from a key's two
  neighbours, so two keys are a straight line and demonstrate nothing about the
  curve — which is the thing CAM is for looking at.
- **The `.FRG` tail is optional by construction.** Entities, the light rig and the
  keys are guarded on remaining payload length in both `forge_io.c` and `frg.py`,
  so a geometry-only file loads and leaves the rest at defaults. `frg.py` keeps
  them as `None` rather than defaulting them, because an importer that invents a
  light rig for a level that never had one is writing content nobody authored.
  The version field is what stops an old file being read as a new one — the first
  time the payload grew without `FRG_VERSION` growing with it, the baked seed
  level loaded as `bad-version` on every boot.
- **The atlas is CI4 to make a level veil-capable by construction, not to save
  space.** 16 tiles of 16x16 in a 64x64 surface, 2 KB against a 4 KB TMEM.
  Swap the TLUT and the whole material changes for 32 bytes of DMA. The
  16-colour palette is also why block types cap at 15 — a palette-swap
  mechanic (a game's own "scarlet veil" or equivalent) can bind straight to a
  Forge level's atlas without any new content pipeline.
