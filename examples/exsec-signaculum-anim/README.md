# exsec-signaculum-anim

The Exsecutor logo's 28-frame rock, **every frame rasterized on the VR4300** by
code the Exsecutor compiler emitted, baked at boot and then played back from
RDRAM — with each frame's framebuffer checked in-ROM against a digest measured
on the host tier.

This is the moving companion to `../exsec-signaculum-demo`, which renders the
single hero view and certifies it. That demo's `EXPECTED_CRC` is **unchanged
and is one of this demo's pins**, so this extends that certificate from one
view to twenty-nine rather than replacing it.

## What the screen shows

A ~15-second bake (`RASTERIZING ON THE VR4300`, a frame counter, a progress
bar), then the logo rocking on a loop with the mode, the cache size, the
measured per-frame cost and the `AGREE n/28` verdict underneath.

The ISViewer verdict lines:

```
signaculum-anim: ram=8192 KiB mode=256x256 cache=3670016 B
signaculum-anim: AGREE 28/28 frames  mean=554845 us worst=559016 us stack=37800/49152 mode=256x256
```

## Why it bakes instead of rendering live

Measured, under Ares, not estimated: **554.8 ms per frame** (worst 559.0 ms)
for 28 consecutive renders. That is 1.8 fps, so live playback is a slideshow.

The same pinned algorithm takes **1.23 ms** on x86-64 — a factor of **451**,
far more than the 43× clock ratio, because `exs_signaculum_pingue` clears and
fills a 196,608-byte framebuffer and a 524,288-byte `f64` 1/z z-buffer every
frame: 720,896 bytes that the clear alone must stream through an 8 KB
write-back D-cache before the first of 2,981 faces is touched. The bake is the
demonstration; the playback is what the bake buys.

Stack high-water is **37,800 of the 49,152-byte kthread**, against the
36,176-byte `-fstack-usage` figure in `../../engine/src/kiln/gen/PROVENANCE.md`.

## The two RAM modes

Chosen by the console, not by a button — `./dev drive` does not work on this
machine (see `flake.nix`'s uinput note), so nothing here waits on input.

| RAM | cache | frames | bytes | verified |
|---|---|---|---|---|
| 8 MB (Expansion Pak) | 256×256 RGBA5551 | 28 | 3,670,016 | `AGREE 28/28`, `mean=554845 us` |
| 4 MB (stock console) | 128×128 RGBA5551, 2×2 box-averaged, pixel-doubled on present | 28 | 917,504 | `AGREE 28/28`, `mean=554748 us` |

The 8 MB plan is **attempted, not assumed**: the engine's own 706 KB of soft3d
statics, libdragon and the display buffers come out of the same pool, so the
full cache is `malloc`'d and the half-resolution plan is the fallback. Whichever
ran is on screen and on ISViewer.

**Both modes AGREE on all 28 frames, and that is the point of where the CRC is
taken**: the digest is over the 256×256 framebuffer *before* it is stashed, so
the RAM mode changes the cache and never the certified bytes. The per-frame cost
is identical in both (554.7 vs 554.8 ms) because only the stash differs.

To exercise the 4 MB path on a machine with the pak, flip `ExpansionPak` to
`false` in the `settings.bml` you pass Ares.

## How the animation works: thirty-six bytes a frame

`prototypes/signaculum_mesh.py`'s EXSG format puts the rotation matrix at a
fixed offset near the front:

```
offset  bytes  field
     0      4  magic "EXSG"
     4      8  2 x u32  vertex count, face count
    12      8  2 x i32  camera d, camera f
    20     36  9 x i32  R = Rx(pitch) @ Ry(yaw), row-major, 2^-23
    56    ...           vertices, then faces
```

Everything from 56 on is the mesh and its baked per-face colours — 44,745 of
44,801 bytes, identical in every view. So `fig_soft3d_set_rotation` patches
**36 bytes** between renders and the animation costs 1,008 bytes of ROM
(`filesystem/rotations.bin`) instead of 1,254,428. The mesh, the camera and the
colours are bit-identical across frames by construction, and every float the
core holds still arrives as an integer on the 2^-23 grid — the property that
makes the framebuffer reproducible across x86-64, qemu-mips64 and the VR4300.

Nothing transcendental runs on the console: the 28 matrices are computed on the
host by EXSECUTOR's `prototypes/signaculum_rotations.py`, whose `Rx @ Ry` and
`q23` are checked against the committed stream's own hero matrix — all nine
integers agree, which is why the two are one convention and not two that look
alike.

## The motion

`logo/README.md`'s GIF row, reproduced exactly:

```
t   = i / 28
yaw = 0.45 * sin(2*pi*t)
pit = -0.14 + 0.05 * cos(2*pi*t)
```

A **rock, not a turntable** — the mark is an extruded flat form, so a full 360°
turns it edge-on and loses it. **Not a palindrome** either: frame 28−i mirrors
the yaw but keeps the pitch, and the mark is asymmetric (one arrow ascending
navy, one descending crimson), so the table is not half a table played
backwards and a consumer cannot store fourteen frames and ping-pong them.

**The hero view is not frame 0.** The committed stream bakes `YAW, PITCH =
0.16, -0.14` (render-logo.py's still); the animation's frame 0 is yaw 0.0,
pitch −0.09. They are different views, which is why this demo pins 29 digests
and not 28.

## Build and run

```sh
nix develop
make -C ../../engine all          # libfigulina.a incl. kiln_soft3d.o
cd examples/exsec-signaculum-anim && make
```

Then boot it. `./dev shot` resolves a ROM through the flake's attributes, not a
path, so until the flake entry lands (below) it cannot be given this `.z64`
directly — `./dev shot examples/.../exsec-signaculum-anim.z64` fails with
`does not provide attribute`. Call the script `./dev shot` wraps instead:

```sh
ARES="$(nix build --no-link --print-out-paths .#ares-bin)/bin/ares" \
  bash tools/n64-shot.sh \
    examples/exsec-signaculum-anim/exsec-signaculum-anim.z64 out.png 30
```

The settle must exceed the bake — at ~15.5 s, a shorter one captures the
progress bar.

For the ISViewer verdict instead of a picture, the sibling demo's Ares recipe
works unchanged; pass a `settings.bml` with `ExpansionPak: false` to exercise
the 4 MB path.

## The host-tier gate

`host_check.c` compiles the **real** `engine/src/kiln/kiln_soft3d.c` against
`plat/host/include`, drives the pinned `signaculum_x86_64.gen.c` through all 28
patches plus the hero view, and compares every CRC against `crc_pins.h`. It
exits non-zero on disagreement, so it is usable as a check as it stands:

```sh
cc -O2 -I plat/host/include -I engine/src/kiln -I engine/include \
   -I examples/exsec-signaculum-anim \
   -o /tmp/hc examples/exsec-signaculum-anim/host_check.c \
   engine/src/kiln/kiln_soft3d.c
/tmp/hc examples/exsec-signaculum-anim/filesystem/signaculum.exsg \
        examples/exsec-signaculum-anim/filesystem/rotations.bin
```

Two snags, both documented in that file: `kiln_soft3d.c` already `#include`s
the generated unit, so passing it to the compiler again is a
multiple-definition error; and the host `<libdragon.h>` macro-replaces stdio,
so a test TU doing its own file IO must `#undef fopen`/`fread`/`fclose`.

This is the pattern `nix/checks/kiln-maprender.nix` and `kiln-splash.nix`
already use — the real module, no ROM, no compositor. Wiring it up as a
`nix/checks/` entry is **not done here**; `grep -n soft3d nix/host.nix` is still
empty, because `kiln_soft3d` is console-tier only today
(`engine/Makefile` sets `N64_CFLAGS` on it). `[OPEN]`

## Not wired into the flake

Like the sibling demo, this ROM has no `nix/demos/` entry yet, so `./dev shot`
is given the built `.z64` by path rather than by name. `flake.nix` is left
untouched deliberately.
