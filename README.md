<!-- SPDX-License-Identifier: MPL-2.0 -->
# M64

A Nix build system for Nintendo 64 / ModRetro M64 software, with a Faust DSP
bridge that enforces the console's numeric limits at build time.

Everything is pinned and hermetic: one `nix develop` gives you a `mips64-elf`
cross toolchain, libdragon, its host tools, Faust, and Ares.

```bash
nix develop              # toolchain + libdragon + tiny3d + engine + faust + ares
nix build .#hello        # -> result/hello.z64
nix build .#engine-demo  # 3D scene + 2D HUD
nix run .#ares -- result/hello.z64
nix flake check          # the gate: toolchain, ROM validity, DSP constraints
./dev shot engine-demo   # boot in Ares on Hyprland, screenshot + pixel stats
./dev doctor             # toolchain / libdragon / cart status
```

Inside the dev shell, a libdragon project builds with a plain `make` — `N64_INST`
and `N64_GCCPREFIX` are already set, so nothing about the normal libdragon
workflow changes.

## What's here

| Output | What it is |
|---|---|
| `packages.toolchain` | `mips64-elf-` GCC 14.4.0 + binutils 2.46 + newlib 4.6.0 |
| `packages.libdragon` | libdragon (`preview`) + host tools (`n64tool`, `mkdfs`, `audioconv64`, `mksprite`, `mkfont`, `n64sym`) |
| `packages.tiny3d` | Tiny3D — RSP-accelerated 3D + `gltf_to_t3d` |
| `packages.engine` | `libm64` — 3D layer on Tiny3D, 2D GUI layer on rdpq |
| `packages.engine-demo` | worked example — lit spinning cube + HUD, 59.8 fps |
| `packages.n64Inst` | the merged `$N64_INST` prefix |
| `packages.hello` | worked example — a bootable `.z64` |
| `packages.audio` | worked example — a ROM that plays a VADPCM-baked instrument |
| `packages.ks-voice` | worked example — a live Faust voice compiled for the VR4300 |
| `packages.ks-baked` | worked example — the same `.dsp` baked to VADPCM offline |
| `packages.sc64deployer` | SummerCart64 upload / `debugf` stdio / IS-Viewer64 |
| `lib.mkN64Rom` | build a libdragon project into a ROM |
| `lib.mkFaustVoice` | compile a `.dsp` into a linkable VR4300 object, gated |
| `lib.mkBakedInstrument` | render a `.dsp` offline at full quality → VADPCM `.wav64` |
| `apps.{ares,cen64,sc64,dev}` | emulators, hardware deploy, the dev CLI |
| `nixosModules.n64-flashcart` | udev rules so deploying doesn't need root |

## The gates

The point of this build system is that the feasibility report's constraints are
enforced rather than remembered. `nix flake check` fails if:

- a Faust voice calls out to libm for a transcendental (`tanhf`, `expf`, `sinf`,
  `powf`, …) — on a 93.75 MHz VR4300 with no FPU datapath this is disqualifying;
  the error points you at `ba.tabulate`;
- any double-precision instruction reaches the ROM (`DIV.D` is 58 cycles against
  a ~2126-cycle per-sample budget at 44.1 kHz);
- the toolchain stops producing big-endian R4000-family code, or newlib's `o64`
  multilib disappears;
- a `.z64` comes out byte-swapped, untitled, or with a non-ASCII header;
- a baked instrument renders silence, peaks below −40 dBFS, or clips. These are
  defects you would otherwise only notice by ear, after they were in a ROM.

The gate inspects **undefined symbols in the compiled object**, not the source.
That matters: `floorf` gets inlined to a single instruction, so grepping the
generated C reports a cost that isn't there, while Faust's internal `fmaxf`
calls never appear in the `.dsp` at all.

The cycle-budget number is a **static estimate** — instruction counts weighted
by the VR4300 latency table. It cannot model cache behaviour or the ~640 ns
RDRAM latency. Use it to catch regressions; profile on hardware for real
numbers.

## The engine

Two layers, one RDP state transition per frame:

```c
m64_frame_begin();            // attach framebuffer + Z-buffer
  m64_scene_begin(&scene);    // 3D: Tiny3D, perspective, lit, depth-tested
  ... geometry ...
  m64_gui_begin();            // the seam: depth OFF, screen space
  m64_gui_panel(...); m64_gui_text(...); m64_gui_bar(...);
  m64_gui_end();
m64_frame_end();              // present
```

The GUI is immediate mode — for a HUD of a dozen rectangles on a 93.75 MHz
VR4300, a retained widget tree costs more than it buys. It uses libdragon's
built-in font, so a ROM needs no filesystem to draw a HUD.

## Hardware

Development targets a real N64 (NUS-001) with a **SummerCart64**. The ModRetro
M64 is a deployment and FPGA-validation target only — it exposes no
cartridge-side USB, so it cannot be used for `debugf`, GDB, or networked play.

```bash
./dev deploy hello     # sc64deployer upload
./dev debug            # debugf() stdio over USB
```

Deploying needs USB permissions:

```nix
imports = [ m64.nixosModules.n64-flashcart ];
programs.n64-flashcart.enable = true;
```

Iterate in Ares, but do not trust any emulator for audio work — the AI's
clock-divider rate, RDRAM contention, and denormal handling are exactly what
emulators get subtly wrong.

## Design notes

See `CLAUDE.md` for orientation and `compass_artifact_*.md` for the feasibility
report that specifies this work. Each file under `nix/` documents why it is
shaped the way it is; `nix/toolchain.nix` in particular records why GCC 14 is
pinned and why libgloss is excluded.

## Licence

MPL-2.0 for the flake and `./dev`. libdragon is Unlicense and SummerCart64 is
GPLv3 — both are packaged as separate programs, not linked into anything here.
Faust `.dsp` sources imported from TERMINUS are PolyForm Shield 1.0.0
(non-commercial) and do not get relicensed by being referenced.
