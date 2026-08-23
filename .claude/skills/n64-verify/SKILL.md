---
name: n64-verify
description: Boot, capture and inspect a Kiln/N64 ROM to find out what it is actually doing — ./dev shot / rec / drive, the debug overlay and kiln_debugdraw's spatial layers, kiln_camlint's camera validator, and how to read pixel statistics. Use when a ROM renders wrong, renders black, or when a change needs confirming on hardware rather than only building; whenever a question is "where is it / is it loaded / is the camera inside the geometry" rather than "does it compile"; and for anything about a cutscene's timing, camera path or keyframes.
---

# Verifying a Kiln ROM

`nix build` proves a ROM links. It proves nothing about what it draws, and on
this hardware the gap between those two is where the expensive bugs live.
Real defects this project has hit that all passed `nix build` and
`nix flake check` cleanly, found only by actually booting the ROM:

| defect | how it looked | how it was found |
|---|---|---|
| A first-person camera applied gravity twice and swept from an already-displaced start, skipping the floor | a playable scene framed high and dark; the player well under the floor | the debug overlay's `eye Y` next to drawn clip brushes |
| `mkRawAsset { name = "foo"; }` shipped `foo.map`; the ROM opened `rom:/maps/foo_bar.map` | a PLAY screen was a black screen with a working HUD | the overlay's `clip 0` |
| `kiln_cache` handle 0 was both "slot 0" and "invalid" | an asset that loaded looked missing | a host unit check |
| `kiln_clip`'s broadphase filled cells with the wrong brushes | the player walked through two of four walls | a flat-vs-grid equivalence sweep |

The order below is cheapest-first. Do not skip to a screenshot.

## 1. Gates first

```bash
nix flake check          # the pre-push gate
```

Includes the two fastest loops in the project:

- **`blender-tests`** — `tools/blender/test_*.py`, every bpy-free geometry
  builder, in milliseconds. `blender_test_*.py` needs a live Blender and is
  deliberately NOT in that glob; the prefix is what makes the exclusion
  intentional. (One test sat failing on a missing `bpy` for a long time
  because nothing ran the glob until this check existed.)
- **`kiln-logic`** — `kiln_clip`, `kiln_dict`, `kiln_cache`, `kiln_lod`,
  `kiln_rng`, `kiln_stream`, `kiln_voxel` compiled natively at `-Werror`
  against `nix/checks/stub/` and asserted on. This is where a new engine
  module's pure logic belongs. Adding one means a stub entry, not a ROM.

Anything that is arithmetic over caller-owned structs should be tested here
rather than looked at. A screenshot cannot tell you a trace fraction is
0.999 instead of 1.

## 2. Boot straight into the screen or mode you care about

`./dev drive` walks a ROM with a virtual pad. That is right for "capture
every screen in order" and wrong for iterating on one screen or mode:
reaching a deep menu/mode the normal way can be a long, unskippable chain of
input.

**Prefer a jump ROM. It needs no controller at all.** Forge's own mode
ROMs are the concrete, real example in this repo:

```bash
nix build .#forge-cam        # boots Forge straight into CAM mode
./dev shot forge-cam out.png 6
```

The pattern generalises to any ROM: build a debug variant that reads a
compile-time flag (`FORGE_MODE=CAM` here) and jumps straight past whatever
input chain normally reaches that screen/mode, because `./dev shot` has no
input path at all by design and `./dev drive`'s uinput→SDL→ares chain is
fragile enough that a capture needing a chord is a capture you cannot trust.

## 3. Read the numbers, not the picture

`./dev shot` prints a colour histogram and a non-black percentage **because
eyeballing is unreliable here**: a 320x240 console upscaled into a large
screenshot of a dim scene reads as "black" when the picture is right there.
That misread has cost this project real time.

Two failure modes the percentage catches immediately:

- **A frame that never changes.** Several shots across a cutscene all
  reporting the same non-black percentage with identical top colours does not
  mean the scene is static; it means **no input reached the ROM** (or the
  capture is not the emulator). Check that before believing anything else.
- **A big move in one number.** This project has more than once fixed a
  missing-collision or camera bug and watched the non-black percentage jump
  by several tens of percentage points in the "after" shot. If a fix is
  real, this number usually says so.

Trust it over your eyes, but **check what the window is** before trusting the
numbers: `tools/n64-shot.sh` matches by the PID it launched, because matching
on the substring `"ares"` once grabbed a YouTube video whose stats looked
perfectly plausible.

## 4. A debug overlay is a pattern to build, not a built-in feature

Kiln ships the pieces (`kiln_console`, `kiln_gui_text`, `kiln_debugdraw`) but
no game-agnostic HUD, because what is worth printing is entirely game-
specific: which state-machine node is live, the camera's actual eye/target,
collision-brush counts, texture/model residency. A game builds its own debug
overlay on these pieces — PetaByte Madness's own `pm_debug.c` (now in that
game's own repo) is a real, worked example of the pattern: it prints the
current screen, camera pose, near/far planes, installed collision-brush
count (red at zero — this is exactly how a "PLAY has no collision world"
defect was caught), and per-model load state (a name in red means "asked
for and absent", not merely "not loaded yet").

The one universal lesson from building that kind of overlay: **give every
capacity or resource count a gauge, and make the gauge red at the value that
means something is lying to you.** A `0` sitting next to plainly-drawn
geometry is a number nobody will believe the next time it's `0` for a real
reason, unless the convention is that it's always supposed to read `0`.

## 5. `kiln_debugdraw` — the spatial half

The engine module that answers *where*, not just *what*. World-space lines,
AABBs, axis gizmos, polylines and labels, drawn through `kiln_scene_project`
in the 2D pass with depth off — so it cannot perturb the frame it describes,
and a brush behind a wall is still visible, which is the point.

- **`kiln_dd_aabb`** — a room's or actor's collision box. The only way to see
  geometry that exists solely as numbers in a C struct.
- **`kiln_dd_axes`** — a named world position or transform, as RGB axis
  gizmos (red +X, green +Y up, blue +Z — **engine** axes, not Blender's,
  since a +Z-up-authored-content-meets-+Y-up-runtime mismatch is a real bug
  class here).
- **`kiln_dd_path`** — a curve. `kiln_camlint`-validated camera paths are the
  motivating case: drawing both the straight-line chord through the keys and
  the actual Catmull-Rom-sampled path in one call reveals spline overshoot as
  a visible bulge, which is invisible as a table of coordinates.
- **`kiln_dd_line`/`_point`/`_text`** — sightlines, markers, and labels for
  anything else worth seeing rather than reading off a struct dump.

Two things to know when reading any of these:

- It is **not free**: dozens of lines plus labels can cost several fps. If
  you are judging frame time, the number with a layer on is not the number
  without it.
- Adding your own is four lines — `kiln_dd_begin(&scene, w, h)` inside
  `kiln_gui_begin`/`_end`, then the draw calls, then `kiln_dd_end()`. Symbols
  are always in `libkiln.a` (the library is built once, without
  `KILN_DEBUG`, so a module that compiled itself away would break every
  debug ROM's link) — gate your *call site*, not the module.

## 6. `kiln_camlint` — validate a camera curve before you ever boot

Most camera bugs are static properties of a keyframe table sitting next to a
dimension the generator already publishes, and `kiln_camlint` checks them
without an emulator. Hard failures — facts, not taste: key times out of
order or duplicated, a key past the curve's declared duration, `eye == look`
(the zero-length view vector that halts the VR4300 inside
`t3d_viewport_attach`, several layers from the table that caused it), and an
inverted frustum. Measured and reported but never fatal, because each is
legitimately intentional somewhere: spline overshoot as a fraction of chord
length, the fastest/slowest segment ratio, and dead tail time.

`nix/checks/kiln-logic.nix`'s coverage of `kiln_camlint` compiles it natively
against `nix/checks/stub/` with a deliberately broken table per rule — the
same discipline any check in this project follows: verified to fire in both
directions, not just seen to pass. PetaByte Madness's own `pm_cine_lint`
(now in that game's own repo) built a full interactive cutscene transport —
pause/step/seek, a cue timeline, a live camera-pose readout for authoring —
on top of exactly this validator; it's the reference for what a game-side
cinematic debugger built on `kiln_camlint`/`kiln_debugdraw` looks like, if
your game needs one.

## 7. Several frames, not one

A single frame cannot show a camera drifting into terrain. For framing and
motion, take a series and montage it:

```bash
for t in 4 8 12 16; do ./dev shot my-jump-rom f$t.png $t; done
montage f*.png -tile 2x2 -geometry +4+4 contact.png
```

`./dev rec <rom> [out.mp4] [dur]` records a clip with Ares-only audio when the
motion itself is the subject.

## 8. When capture is not trustworthy

Two environment failures look like ROM bugs and are not:

- **Ares maps a window whose Vulkan surface never composites.** Every geometry
  check in `tools/n64-shot.sh` passes and `grim` captures whatever is behind it.
  Tell-tale: the capture shows a desktop window, or the same pixels regardless
  of the ROM.
- **`./dev drive`'s input chain does not take.** The uinput pad is created, SDL
  is probed, controls are bound, ares finds them — and no button reaches the
  ROM. Tell-tale: an on-screen timer advances while nothing responds.
  `tools/n64-drive.sh`'s header is largely about how fragile this chain is.

Both are why jump ROMs exist. If capture is unreliable in your environment,
say so and fall back to `nix build` plus the gates rather than reporting a
capture you do not trust.

## 9. On hardware

Emulators reconstruct the AI clock divider and RDRAM contention inexactly, so
**never trust one for audio work**. `./dev deploy <rom>` uploads to a
SummerCart64 and `./dev debug` reads `debugf()` over USB — both unverified
against a physical cart in this project so far, so treat a first run as
testing the tooling too. Note that `debugf` goes nowhere without a cart
attached, which is why the diagnostics above are on screen and not in a log.
