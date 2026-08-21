---
name: n64-verify
description: Boot, capture and inspect a Kiln/N64 ROM to find out what it is actually doing — ./dev shot / rec / drive / cine, the pm-jump-<screen> ROMs, the debug overlay and kiln_debugdraw's spatial layers, pm_cine's cutscene transport (pause/step/seek, the cue timeline, the PMCamKey pose readout) and camera validator, and how to read pixel statistics. Use when a ROM renders wrong, renders black, or when a change needs confirming on hardware rather than only building; whenever a question is "where is it / is it loaded / is the camera inside the geometry" rather than "does it compile"; and for anything about a cutscene's timing, camera path or keyframes.
---

# Verifying a Kiln ROM

`nix build` proves a ROM links. It proves nothing about what it draws, and on
this hardware the gap between those two is where the expensive bugs live. Four
of the five worst defects found in PetaByte Madness passed `nix build` and
`nix flake check` cleanly:

| defect | how it looked | how it was found |
|---|---|---|
| `kiln_fpscam` applied gravity twice and swept from an already-displaced start, skipping the floor | the playable lab framed high and dark; the player 1.7 m under it | the debug overlay's `eye Y` next to drawn clip brushes |
| `pmLabMap` shipped `maps/pm-lab-map.map`; the ROM opened `rom:/maps/pm_lab.map` | PLAY was a black screen with a working HUD | the overlay's `clip 0` |
| `kiln_cache` handle 0 was both "slot 0" and "invalid" | an asset that loaded looked missing | a host unit check |
| `kiln_clip`'s broadphase filled cells with the wrong brushes | the player walked through two of four walls | a flat-vs-grid equivalence sweep |

The order below is cheapest-first. Do not skip to a screenshot.

## 1. Gates first

```bash
nix flake check          # 59 checks; the pre-push gate
```

Includes the two fastest loops in the project:

- **`blender-tests`** — `tools/blender/test_*.py`, every bpy-free geometry
  builder, in milliseconds. `blender_test_*.py` needs a live Blender and is
  deliberately NOT in that glob; the prefix is what makes the exclusion
  intentional. (It was `test_rider.py` for a long time, failing on a missing
  `bpy` that nobody saw, because nothing ran the glob.)
- **`kiln-logic`** — `kiln_clip`, `kiln_dict`, `kiln_cache`, `kiln_lod`, `kiln_rng`
  compiled natively at `-Werror` against `nix/checks/stub/` and asserted on.
  This is where a new engine module's pure logic belongs. Adding one means a
  stub entry, not a ROM.

Anything that is arithmetic over caller-owned structs should be tested here
rather than looked at. A screenshot cannot tell you a trace fraction is
0.999 instead of 1.

## 2. Boot straight into the screen you care about

`./dev drive` walks the game with a virtual pad. That is right for "capture
every screen in order" (`tools/drive/intro.txt`) and wrong for iterating on one
shot: reaching INTAKE the normal way is ~90 s of splash, title, attract idle,
file select, narration crawl and a walk across the lab.

**Prefer a jump ROM. It needs no controller at all:**

```bash
nix build .#pm-jump-intake        # boots straight into INTAKE
./dev shot pm-jump-intake out.png 6
```

One per screen: `pm-jump-{title,file,narration,lab-cine,lab,intake,credits,sub,beach,play}`.
Each is `petabyte-madness-debug` plus `PM_JUMP=<SCREEN>`, and each turns on the
spatial overlay layer that screen is worth inspecting with (`flake.nix`'s
`pmJumpScreens`).

The debug ROM also opens on an interactive jump menu (D-pad + A; START boots
normally; it auto-dismisses after 12 s so it can never strand the ROM).

**Do not hand-patch forced transitions into `pm_screens.c`.** That is what the
jump exists to replace, and the version that was there wrote save slot 0 on
every boot as a side effect of bypassing the file screen.

## 3. Read the numbers, not the picture

`./dev shot` prints a colour histogram and a non-black percentage **because
eyeballing is unreliable here**: a 320x240 console upscaled into a large
screenshot of a dim scene reads as "black" when the picture is right there.
That misread has cost this project real time.

Two failure modes the percentage catches immediately:

- **A frame that never changes.** Ten shots across a 14-second cutscene all
  reporting 5.37% non-black with identical top colours does not mean the
  cutscene is static; it means **no input reached the ROM** (or the capture is
  not the emulator). Check that before believing anything else.
- **A big move in one number.** 5.55% → 57.55% is what fixing PLAY's missing
  collision looked like. 8.98% → 48.91% is what fixing the fpscam fall looked
  like. If a fix is real, this number usually says so.

Trust it over your eyes, but **check what the window is** before trusting the
numbers: `tools/n64-shot.sh` matches by the PID it launched, because matching on
the substring `"ares"` once grabbed a YouTube video whose stats looked perfectly
plausible.

## 4. The debug overlay — the text half

Build `-debug` (or any `pm-jump-*`). `pm_debug.c` prints, in order:

```
LAB 5.9s  fade 0.00  61 fps  dd:clip 12/124
eye     60    104    120  ->   -40    104    120
near 10 far 200 fov 85  veil 0.00
fps-cam    60   104    120 yaw -1.57 grounded  clip 8
isle palm - - lab - arms - logo sky sea
```

What each line settles that a picture cannot:

- **screen + t** — which state machine node is live. (Generated from
  `PM_SCREEN_LIST`; it used to be a hand-kept array and it drifted twice.)
- **`dd:<set> drawn/clipped`** — the spatial layers. `0/0` means no layer is on;
  `0/124` means a layer is on and **everything is behind you**. Identical on
  screen, completely different problems.
- **eye / target** — the camera the scene was *actually built from*, whichever
  module wrote it. This is how "the camera is inside the geometry" gets answered.
- **near / far** — red when a wide shot's far plane is under 2000, which IS the
  veil-clamps-everything defect and is invisible in a picture because the island
  simply is not drawn.
- **`clip <n>`** — installed collision brushes, **red at zero**. An empty clip
  world is legitimate, so nothing else can report it; that is why PLAY fell
  forever for its whole life.
- **`veil-pal n/3`** — baked veil palettes loaded. Red at zero: the filter
  degrades silently to fog plus a far-plane rebate, which looks like the whole
  effect if you have not seen the other half.
- **model residency** — a dash is "never asked for", a name is loaded, a name in
  red is **asked for and absent**. A model that resolved to NULL draws nothing,
  which on a dark screen is indistinguishable from a camera pointed the wrong
  way, and telling those apart by reading source has cost days.

## 5. The debug overlay — the spatial half (`kiln_debugdraw`)

The half that answers *where*. Four layers, cycled with **C-Up + C-Down**, or
selected at build time with `PM_DD=<0..4>` (`0` off, `1` cam, `2` clip,
`3` actors, `4` all) — the build flag exists because a capture run cannot press
a chord.

- **`clip`** — the room's collision boxes. The only way to see geometry that
  exists solely as numbers in a C file.
- **`actors`** — every live actor's position and facing, coloured by category.
  "The guards spawned as PROPs" becomes visible without reading a profile table.
- **`cam`** — the playing shot's camera, drawn as **two** curves: dim is the
  straight-line chord through the keys, bright is the path the camera actually
  flies (sampled from `pm_camkey_sample`, the same function that feeds the
  camera). **The gap between them is the Catmull-Rom overshoot** — invisible as a
  table of eye coordinates, obvious as a bulge. The orange curve is the LOOK
  path, which overshoots the same way and is usually what makes a camera feel
  drunk. Plus a sightline from each eye key to its target: where one crosses a
  wall, the shot opens inside geometry.
- **`anchors`** — the named world positions the game is keyed to, as RGB axis
  gizmos (red +X, green +Y up, blue +Z — **engine** axes, not Blender's, since
  the bug class is a +Z-up convention meeting a +Y-up runtime).

Two things to know when reading it:

- It draws in **screen space** through `kiln_scene_project`, in the 2D pass, with
  depth off. So it cannot perturb the frame it is describing, and a brush behind
  a wall is still visible — which is the point.
- It is **not free**: ~37 lines plus labels takes 60 fps to 54. If you are
  judging frame time, the number with a layer on is not the number without it.

Adding your own is four lines — `kiln_dd_begin(&scene, w, h)` inside
`kiln_gui_begin`/`_end`, then `kiln_dd_aabb` / `_line` / `_axes` / `_path` /
`_point` / `_text`, then `kiln_dd_end()`. Symbols are always in `libkiln.a`
(the library is built once, without `KILN_DEBUG`, so a module that compiled
itself away would break every debug ROM's link); gate your *call site* the way
`pm_debug.h` gates its whole module.

## 6. Cutscenes — the transport, the timeline and the pose readout

Everything above answers "what is the ROM doing". A cinematic needs a fifth
question answered first: **at what time?** `pm_cine` (PetaByte-Madness/src/
pm_cine.h) makes the shot clock addressable, and it is the difference between
inspecting a cutscene and waiting for one.

```bash
nix build .#pm-cine-intake && ./dev run pm-cine-intake
```

**L + R arms the transport** and takes the pad with it — deliberately, because
otherwise START skips the shot you are studying. Then:

| | |
|---|---|
| `START` | pause / resume |
| `R` | step one frame |
| `L` | cycle rate 0.25x / 0.5x / 1x / 2x |
| `D-left` / `D-right` | seek -0.5 s / +0.5 s |
| `D-up` / `D-down` | seek to the previous / next keyframe |
| `Z` | detach the free-fly camera |

The map is drawn on screen for the first few seconds after arming, so it does
not have to be remembered.

**Seeking re-runs the shot; it does not rewind it.** A cutscene here is a curve
plus an animation state machine plus fx plus sound, and a state machine cannot
be run backwards. So a seek restarts the shot and pumps it forward at a fixed
1/60 step with SFX muted. It costs a visible hitch on a long shot and buys the
property that matters: **the same time is the same frame, every run, on any
machine**.

### The timeline strip

Along the bottom: the duration as a bar, keyframes as white ticks, **cues as
amber ticks**, and the playhead. Plus `intake  t 12.34 of 26.06  x0.50 PAUSE
k4 of 17  f 0.62` — which key is live and how far through its segment.

The cue ticks are the answer to "did the sound fire before the motor". They are
collected from funnels, not from per-cutscene instrumentation: every
`pm_fx_shake` / `_flash` / `_hitstop` / `_letterbox`, every `pm_sfx_play`, and
every animation clip change lands on the timeline automatically. The last three
are named underneath it.

### The pose readout — how to author a key

Press `Z` and fly (stick moves, C buttons look, A/B rise and fall; the speed
scales itself off the shot's own far plane). The shot keeps running underneath;
you are just no longer looking through its camera, which is drawn as a gizmo
with **its frustum** so "the far plane cuts the temple" is a picture rather than
two numbers.

While detached the overlay prints, continuously:

```
KEY t 12.34 eye 120 88 -14 look -30 70 5
```

That is a `PMCamKey` in the order the table wants it. Printed continuously
rather than latched behind a button, because then **any screenshot of free-fly
mode carries the numbers** — which is what makes this loop work from a capture
and not only from a controller: see the curve bulge through a wall, fly to where
the shot should be, read the line off the PNG, paste it into the table.

Deliberately plain text and not a C literal, and the readout stays
alphanumeric plus `.` and `-`. The reason is misreading, not a missing glyph:
`FONT_BUILTIN_DEBUG_MONO` does have a `@` (and every other printable ASCII
codepoint — `nix/checks/kiln-font.nix` pins that), but at a 6 px advance `@`
looks like a decorated `0`, which is how a `2@5.4` in this project's history
got transcribed as `205.4`. Anything you intend to retype off a capture should
avoid the characters that collide at that size.

### Deterministic capture

`PM_SHOT_AT=<seconds>` boots, seeks to that **shot** time, and pauses. This is
the replacement for `./dev shot <rom> out.png 6`, whose six seconds are wall
clock and therefore a guess about emulator speed and machine load.

**Two captures of one `PM_SHOT_AT` ROM are byte-identical — after you crop the
12-pixel window border.** Verified, and both halves of that sentence were
earned:

- The debug overlay's uptime and frame-rate fields cannot be the same twice, so
  a `PM_SHOT_AT` build prints them as `--s` and `--` (`pm_cine_repro()`, read by
  `pm_debug.c`). Before that they were the *entire* difference between two
  captures: 319 pixels, all in those two fields.
- What remains is 37 pixels of compositor chrome on the outer rows of the
  window, which is not emulator output at all. Crop 12 px on every side and the
  diff is exactly zero.

So diff cropped, not raw. A raw `md5sum` of two good captures will differ and
tell you nothing:

```python
box = (12, 12, w - 12, h - 12)          # then compare a.crop(box) to b.crop(box)
```

`./dev cine <screen> [n]` gives the whole shot as a contact sheet from one boot:
it builds `pm-ladder-<screen>`, which walks the shot in `n` evenly-spaced rungs
held for a fixed number of *frames*, records it, and extracts the stills. The
overlay stamps each frame with its shot time, so the sheet stays readable even
if extraction timing drifts — **make the frame self-describing rather than the
capture perfectly timed**. That is not hypothetical: on a six-frame sheet two
extractions landed on the same rung, and because both frames say `t 19.28` the
duplicate is visible instead of passing as two different moments.

If `gpu-screen-recorder` cannot start (it wants a KMS server with
`CAP_SYS_ADMIN`, and the `pkexec` route dies outright when `$SHELL` is not in
`/etc/shells`), `./dev cine` falls back to one boot per frame via `./dev shot`.
Slower, same sheet — the rungs are frame-counted and self-labelling, so a settle
that lands early still names the moment it caught.

## 7. The camera validator — before you boot at all

Most camera bugs in this project were static properties of a keyframe table
sitting next to a dimension the generators already publish. `pm_cine_lint`
checks them without an emulator:

```bash
./dev cine-lint                     # the report over every registered shot
```

Hard failures — facts, not taste: key times out of order or duplicated, a key
past the shot's `duration` (unreachable: `pm_demo_update` clamps there),
`eye == look` (the zero-length view vector that halts the VR4300 with a
floating-point invalid operation, several layers from the table that caused it),
an inverted frustum, and **`|look - eye| > far_z`** — the shot's own subject
behind its own far plane, which is the class `pm_demo.h` records as having "cost
six shots their geometry".

Measured and reported but never fatal, because each is legitimately intentional
somewhere: spline overshoot as a fraction of chord length, the fastest/slowest
segment ratio, an eye outside the lab's measured box, and dead tail time. Each
row's second line ends in the **key index** — `overshoot hitch k11` — because
"overshoot 1.43" is a fact you then have to bisect by hand and `k11` names the
two keys to look between.

A shot with no `.draw` callback has no 3D pass (`pm_demo_draw` no-ops), so it
has no camera to validate and is listed as `no 3D pass` rather than linted —
NARRATION and CREDITS are both. That is derived from the shot, not declared, so
giving one of them a `.draw` starts linting it with no edit to `pm_cine.c`.

**Its first run over the real tables found a real defect**, which is the whole
argument for the check: `INTAKE` had two keys at the same instant, because
`INTRO_T` and `T2(45)` are the same number by construction
(`ORIG_HANDOFF_FRAME` is 45). `pm_camkey_sample`'s bracketing scan walks past a
zero-span segment, so that key was never flown *through* — but Catmull-Rom takes
its tangent from a key's two **neighbours**, so it still bent the curve either
side of the seam while being unreachable itself. Dead data that is not inert,
invisible to every capture, and a one-line report here.

The detector itself is gated in `nix flake check` (`nix/checks/pm-cine.nix`),
compiled natively against `nix/checks/stub/` with one deliberately broken table
per rule. The real tables are linted on console instead, because they cannot
leave the ROM build — and because `FLYOVER_KEYS` is not a compile-time constant
at all: `flyover_build_keys` fills it during `setup`, so the lint ROM plays each
shot's `setup()` before reading its keys.

## 8. Several frames, not one

A single frame cannot show a camera drifting into terrain. For framing and
motion, take a series and montage it:

```bash
for t in 4 8 12 16; do ./dev shot pm-jump-intake f$t.png $t; done
montage f*.png -tile 2x2 -geometry +4+4 contact.png
```

`./dev rec <rom> [out.mp4] [dur]` records a clip with Ares-only audio when the
motion itself is the subject.

## 9. When capture is not trustworthy

Two environment failures look like ROM bugs and are not:

- **Ares maps a window whose Vulkan surface never composites.** Every geometry
  check in `tools/n64-shot.sh` passes and `grim` captures whatever is behind it.
  Tell-tale: the capture shows a desktop window, or the same pixels regardless
  of the ROM.
- **`./dev drive`'s input chain does not take.** The uinput pad is created, SDL
  is probed, 18 controls are bound, ares finds them — and no button reaches the
  ROM. Tell-tale: the on-screen timer advances while nothing responds.
  `tools/n64-drive.sh`'s header is largely about how fragile this chain is.

Both are why the jump ROMs exist. If capture is unreliable in your environment,
say so and fall back to `nix build` plus the gates rather than reporting a
capture you do not trust.

## 10. On hardware

Emulators reconstruct the AI clock divider and RDRAM contention inexactly, so
**never trust one for audio work** (report §7). `./dev deploy <rom>` uploads to
a SummerCart64 and `./dev debug` reads `debugf()` over USB — both unverified
against a physical cart, so treat a first run as testing the tooling too. Note
that `debugf` goes nowhere without a cart attached, which is why the diagnostics
above are on screen and not in a log.
