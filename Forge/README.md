<!-- SPDX-License-Identifier: MIT -->
# Forge — the level editor that runs on the console

`nix build .#forge`. On an unfamiliar flashcart, run `.#forge-selftest` first:
it reports whether there is a writable backend at all, and it is supposed to
report *none* under an emulator.

A Minecraft-shaped voxel builder whose save button emits the formats this engine
already consumes. It exists because the judgements that matter on this hardware —
fill rate, whether a palette still separates once the veil discards hue, whether
a corridor reads as a corridor — cannot be made two hops away on a desktop.

Read `.claude/skills/n64-forge/SKILL.md` for the whole thing. This is the map.

## The loop

```
nix build .#forge && cp -L result/forge.z64 "$(./dev forge-card)/"
./dev forge-push assets/oot_test.map     # a .map onto the card, as .FRG
                                          #   ... edit on the console, START ...
./dev forge-pull "" my_level             # .FRG + .MAP + .H + .KEY into assets/
```

The ROM is built once; levels are data on the SD card, so editing costs no
rebuild and no reflash. **USB is dead on an ED64 Plus** (`libdragon/src/usb.c`
rejects EverDrive 2.5-class boards), so every diagnostic is on screen or in
`FORGE/FORGE.LOG` on the card.

## Six modes, in authoring order

`L+R` cycles. Each has a jump ROM — `nix build .#forge-<geo|walk|paint|ent|light|cam>` —
because `./dev shot` has no input path, so a mode reached only by a chord could
otherwise only be verified by hand.

| mode | authors | out |
|---|---|---|
| GEO | blocks: place, dig, drag-fill, 15 types | the brushes |
| WALK | nothing — you stand in it under the real `kiln_fpscam` | — |
| PAINT | the CI4 atlas; `Z` previews the **veiled** palette | `.FRG` |
| ENT | classname, origin, angle, numeric epairs | `.map` point entities |
| LIGHT | key/fill direction and level, ambient, fog, clear colour | a generated header |
| CAM | keyframes, validated live by `kiln_camlint` | a `PMCamKey` table |

**The controls live in `src/forge_binds.def`** and nowhere else. `forge_hud.c`'s
in-ROM help and the skill's table both come from it —
`python3 tools/forge/gen_binds.py`. They used to be three independent tables and
all three claimed GEO fills with `Z+A`, which it never has.

## Files

| file | what |
|---|---|
| `src/forge.h` | the one `Forge` struct, every cap, every prototype |
| `src/forge_main.c` | init, the mode switch, the frame loop |
| `src/forge_geo.c` | reticle, place/dig/drag-fill, the mesh cache |
| `src/forge_walk.c` | greedy boxes into the real clip world, real `kiln_fpscam` |
| `src/forge_paint.c` | the 16×16 CI4 tile editor and its palette |
| `src/forge_ent.c` | the classname picker and the `.map` entity emitter |
| `src/forge_light.c` | the 7-field light rig and its generated header |
| `src/forge_cine.c` | keyframes, scrubbing, live `kiln_camlint` |
| `src/forge_io.c` | `.FRG` encode/decode, the `.MAP` emitter, the aux writers |
| `src/forge_map.c` | one AABB brush as `.map` text — **compiles natively**, so `level-vocab` can assert on its winding |
| `src/forge_binds.def` | the control scheme |
| `src/forge_vocab.gen.h` | **generated** from `tools/schema/level_vocab.json` |

`tools/forge/frg.py` is the host mirror: `info`, `tomap`, `frommap`, `selftest`.
`nix/checks/forge-roundtrip.nix` runs its selftest and then holds a `.map` to a
byte-identical round trip through it.

## Two things that will surprise you

- **The atlas is not currently reaching the screen.** `kiln_voxmesh` puts the
  block type in the UVs, but `kiln_scene_begin` sets `RDPQ_COMBINER_SHADE`,
  which outputs vertex colour and discards the texel — so every block type draws
  the same grey. The fix is one `rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE)`
  **in Forge**, and `nix/checks/kiln-voxmesh.nix` holds both captures.
- **`sram_detect()` returns 0, not −1, when there is no save chip**, so a `< 0`
  test can never fail. `try_sram` requires a positive size *and* a round-trip.
