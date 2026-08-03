# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A **Nix build system for Nintendo 64 / ModRetro M64 software**, with a Faust DSP
bridge. It exists to implement `compass_artifact_wf-…_text_markdown.md` — a
feasibility report on running Faust DSP on the N64 and porting the SSHitunneller!
game to it. **That report is the spec.** Read it before proposing anything; it
carries its own caveats section about what is unverified.

The central design idea: the report is a set of numeric constraints that are
easy to violate silently and expensive to discover on hardware, so the build
system enforces them rather than documenting them. A `.dsp` that calls libm, or
a voice that emits a double-precision instruction, fails `nix build`.

Sibling repos under `~/Documents/`, all separate checkouts (not submodules):
`DeMoD` (the Faust corpus, Quanta, `dm.dcf`), `ssh-dungeon-rpg` (SSHitunneller!
itself — Lua game + Rust `russh` transport), `HydraMesh` (the certified
`DeModFrame`/DCF-Audio/SuperPack wire).

## Commands

```bash
nix develop            # toolchain + libdragon + faust + ares; sets N64_INST
nix build .#hello      # -> result/hello.z64
nix run .#ares -- result/hello.z64
nix build .#engine-demo  # 3D + 2D GUI worked example
nix flake check        # the pre-push gate — see "The gates" below
./dev shot engine-demo out.png   # boot in Ares on Hyprland, capture + pixel stats
./dev doctor           # toolchain / libdragon / cart status
./dev deploy hello     # sc64deployer upload to a SummerCart64
./dev debug            # debugf() stdio over USB
```

Inside `nix develop`, a libdragon project builds with a **plain `make`** —
`N64_INST` and `N64_GCCPREFIX` are already exported. Verified working.

## Architecture

```
nix/toolchain.nix   mips64-elf- prefix farm over pkgsCross.mips64-embedded.
                    Owns the ENTIRE toolchain strategy — the only file that
                    knows where the compiler comes from.
       ▼ N64_GCCPREFIX
nix/libdragon.nix   ONE native derivation that invokes the cross compiler for
                    part of its work (mirrors upstream's build.sh): host tools
                    with $(CC), target lib with $(N64_CC).
nix/tiny3d.nix      Tiny3D, installed with libdragon's layout.
nix/engine.nix      libm64 — the M64 engine (3D on Tiny3D, 2D GUI on rdpq).
nix/n64-inst.nix    symlinkJoin of the above into ONE $N64_INST prefix.
                    Built in two stages: libdragon+tiny3d (what the engine
                    compiles against), then +engine (what ROMs consume).
       ▼ N64_INST
nix/rom.nix         mkN64Rom — supplies the hermetic environment; the PROJECT
                    brings a Makefile (see the file for why).
nix/assets.nix      mkModel/mkSprite/mkFont/mkSound/mkMusic/mkRawAsset — wrap
                    the merged prefix's host tools (gltf_to_t3d, mksprite,
                    mkfont, audioconv64, mkasset). Each produces a
                    `filesystem/` directory in mkBakedInstrument's convention,
                    so mkN64Rom's `assets` list consumes them unchanged.
nix/faust.nix       mkFaustVoice / mkBakedInstrument, plus the gates.
dsp/arch/           Faust architecture files, written from scratch against
                    Faust's C ABI (libdragon_mixer.c = live, offline_ref.c =
                    full-quality host render).
tools/n64-shot.sh   boot a ROM in Ares on Hyprland and grim its window.
```

Two prefixes, deliberately distinct: **`N64_GCCPREFIX`** is the toolchain,
**`N64_INST`** is the library prefix. `n64.mk`'s `N64_GCCPREFIX ?= $(N64_INST)`
override exists for exactly this case, so the compiler never has to be merged
into the library tree. `N64_INST` itself *is* a symlinkJoin, because libdragon,
Tiny3D and libm64 all expect to be installed into one prefix (Tiny3D's
`t3d-inst.mk` literally does `-lt3d`, which only resolves if `libt3d.a` sits
beside `libdragon.a`) and Nix store paths are immutable.

## The engine (engine/, examples/engine)

Two layers, one state transition per frame:

```
m64_frame_begin()          attach framebuffer + Z-buffer
  m64_scene_begin(&scene)  3D: Tiny3D, perspective, lit, depth-tested
    ... draw geometry ...
  m64_gui_begin()          <- the seam: depth OFF, standard combiner
    ... panels/text/bars ...  2D: rdpq, screen-space
  m64_gui_end()
m64_frame_end()            present
```

The split is not cosmetic — the RDP is a state machine and the two passes want
opposite configurations. Drawing HUD inside the 3D pass costs per-pixel depth
compares on something that can never be occluded, and lets geometry in front
z-reject it. The bracket makes that mistake obvious.

The GUI is **immediate mode** on purpose, and this is the opposite of DeMoD
UI's retained `DmWidget` tree. A retained tree buys layout and hit-testing at
the cost of per-widget allocation and a tree walk per frame; for a dozen HUD
rectangles on a 93.75 MHz VR4300 that trade is the wrong way round. Different
machine, different answer — do not "port" the DeMoD widget model here.

Text colour goes through libdragon's *style* system (colours are interned into
style slots on first use), because `rdpq_text_print` takes a style id, not a
colour. Sixteen slots, then fallback to white.

Verified: `examples/engine` runs at **59.8 fps** in Ares with a lit spinning
cube plus HUD.

## Screenshots — how ROMs actually get verified

`./dev shot <rom> [out.png] [settle]` boots the ROM in Ares **on the live
Hyprland session** and captures the window with `grim`, then prints pixel
statistics. Headless was tried and abandoned: under SDL's dummy video driver
the Vulkan-backed N64 renderers cannot create a surface at all (gopher64:
"Vulkan support is either not configured in SDL or not available in current
video driver (dummy)").

Two things that will waste your time otherwise, both handled in
`tools/n64-shot.sh`: the ROM must be copied somewhere **writable** first (given
a Nix store path, Ares opens a read-only-save modal and sits there, so you
screenshot a dialog), and `--settings-file` must point at a throwaway config or
a screenshot run mutates the user's real Ares settings.

**Trust the pixel statistics, not your eyes.** A 640x240 console upscaled into
a 2521x1561 screenshot of a mostly-black screen reads as "black" when the text
is right there — that misread cost real time during development. `./dev shot`
prints colour histograms and a non-black percentage precisely so that judgement
is not visual.

**But pixel statistics only help if the window is actually Ares.** The window
matcher used to look for the substring `"ares"` anywhere in a client's
class+title, which also matches an unrelated window whose title merely
contains those five letters — "prepares", "shares", "declares" all qualify.
It once silently grabbed a YouTube video playing in another window instead of
the emulator, and the pixel stats for that frame looked perfectly plausible
(77% non-black) right up until someone looked at the PNG. `tools/n64-shot.sh`
now matches by the PID it just launched, which is unambiguous, with an exact
(not substring) class-name fallback. If a capture ever looks wrong, check
which window it actually is before trusting the numbers.

## Hard-won facts (do not re-derive these)

Each cost real build time to discover. `nix/toolchain.nix` documents them inline.

- **GCC 14 is pinned deliberately.** nixpkgs' default (15.3.0) ICEs building
  libstdc++ for mips64 (`expand_fn_using_insn, at internal-fn.cc:268`).
  libdragon's own `build-toolchain.sh` pins `GCC_V=14.4.0`, which is exactly
  nixpkgs' `gcc14`. Two reasons pointing the same way.
- **libgloss is excluded from newlib.** nixpkgs deliberately re-enables it for
  cross targets; on mips64 it fails to assemble with binutils 2.46
  (`crt0.S:92: Error: invalid operands 'mtc0 $0,C0_CAUSE'`). libdragon supplies
  its own crt0/syscalls (`entrypoint.S`, `libdragonsys.a`, `n64.ld`) and links
  only `-ldragon -lm -ldragonsys`, so libgloss is not wanted.
- **The ABI is `o64`**, not n64 or n32 — that is what `n64.mk` uses. It links
  `elf32-bigmips`, which is correct: o64 is a 32-bit-address ABI with 64-bit
  registers. The newlib `o64` multilib exists and links; this was the single
  biggest risk and it is retired.
- **libdragon is pinned to the `preview` branch, not `trunk`.** Tiny3D requires
  it and typedefs its math types straight off libdragon's fast-math vectors
  (`typedef fm_vec3_t T3DVec3;`). On trunk those typedefs resolve to nothing
  and every use fails with errors that look unrelated ("control reaches end of
  non-void function", "request for member 'm' in something not a structure").
- **libdragon preview needs a `struct stat` patch on MIPS.** newlib hard-codes
  a legacy layout for `__mips__` (scalar `st_mtime`, no `st_mtim` timespec) and
  `src/fat.c` assumes modern POSIX. No feature-test macro reaches the timespec
  branch; two lines are patched in `nix/libdragon.nix`.
- **Third-party `-Werror` has to be neutralised, and deleting it is not
  enough.** libdragon's own `n64.mk` puts `-Wall -Werror` into
  `N64_C_AND_CXX_FLAGS`, which lands *earlier* on the command line than
  anything a downstream library appends — so Tiny3D fails on warnings coming
  from libdragon's headers. `nix/tiny3d.nix` substitutes a trailing
  `-Wno-error` in rather than removing Tiny3D's own flag.
- **`-mfix4300` is not used.** The report claims it should be, but it appears
  nowhere in libdragon and is not a stock GCC flag. Do not add it back without
  checking `gcc --help=target` first.
- **`-mem` cannot be used with Faust's C backend** (Faust rejects it outright).
  Not needed anyway: the architecture file holds one static instance and never
  calls `new`/`delete`, and `--gc-sections` drops them.
- **`-ftz 2` is broken in Faust 2.85.9's C backend** — at both precisions, in
  both block and one-sample mode. The C++ backend emits
  `*reinterpret_cast<int64_t*>(&x)`; the C backend lowers that to
  `*((int64_t*(&x)`, dropping the cast's parentheses, which does not parse.
  We use **`-ftz 1`** (fabs-based, same semantics, `abs.s` = 1 cycle). The
  report's `-double -ftz 2` house style is therefore not reachable via
  `-lang c`. Single source of truth: `ftzMode` in `nix/faust.nix`.
  Adding it cost the KS voice 291 → 333 weighted cycles; that is the price of
  not trapping into the denormal exception handler.
- **Faust `-os` emits `frame()` and leaves `compute()` an EMPTY STUB.** An
  architecture file written against `compute()` builds, links, runs, and outputs
  silence.
- **ROM titles need literal quotes** in the make variable (`N64_ROM_TITLE =
  "M64 Hello"`) because `n64.mk` interpolates `n64tool -t $(N64_ROM_TITLE)`
  unquoted. Without them a title with a space fails with "Need output flag
  before first file".
- Cross artifacts need `dontStrip` / `dontPatchELF`; nixpkgs' fixup phase
  otherwise strips the debug ELF that `n64sym` needs for backtraces.
- **`gltf_to_t3d` aborts on a glTF material with no fast64 data** ("Material
  has no fast64 data! (@TODO: implement fallback)", `terminate called after
  throwing... std::runtime_error`). It expects the custom properties
  Blender's fast64 add-on writes on export; a hand-authored or
  otherwise-exported glTF needs `mkModel`'s `ignoreMaterials = true` (passes
  `--ignore-materials`) or it never gets past import. `assets/cube.gltf` and
  `nix/assets.nix`'s `demoModel` hit this first.

## The gates (nix/faust.nix, nix/checks/)

Verified to fire in both directions — a clean voice passes, a voice using
`ma.tanh` fails with an actionable message.

The no-libm gate inspects **undefined symbols in the compiled object**, not
source text. This is deliberate and better: `floorf` gets inlined to one
instruction (a source grep would report a cost that isn't there), while Faust's
internal `fmaxf`/`fminf` never appear in the `.dsp` at all. `-ffast-math`
(which libdragon enables) is what inlines them.

The cycle-budget number is a **static estimate** — instruction counts weighted
by the report's §2 VR4300 latency table. It cannot model cache or the ~640 ns
RDRAM latency. It exists to catch regressions, not to predict wall-clock. Say
so whenever quoting it; profile with `TICKS` on hardware for real numbers.

## Constraints that shape everything

- **Single precision only.** No `-double` on-console. That flag is for offline
  reference renders on the host.
- **The N64 cannot terminate SSH.** The port's architecture (report §6) is a
  host-PC proxy that unpacks DCF-Audio codec_id 2 frames and forwards *control
  data only* over flashcart USB; the cart synthesises locally. Audio is never
  streamed to the console.
- **The M64 has no cartridge-side USB**, so it can never be a live networked
  client — it is a deployment/QA box. Development needs a NUS-001 + SummerCart64.
- **Bake before you synthesise.** Report Stage 1 (offline render →
  `audioconv64` → VADPCM `.wav64`) is meant to carry 80–90% of the audio. Do not
  propose a pure real-time on-console architecture; the report rules it out.

## The two audio paths (both built, both first-class)

Report §5's recommendation is hybrid: bake 80–90%, keep live synthesis for what
must be parametric. Both are implemented from the same `.dsp`:

```
dsp/ks.dsp
  ├─ mkBakedInstrument  -> host render (-double) -> audioconv64 -> .wav64
  │                        -> mkN64Rom `assets` -> DragonFS -> RSP mixer
  │                        (packages.ks-baked, examples/audio)
  └─ mkFaustVoice       -> faust -lang c -single -os -> VR4300 object
                           (packages.ks-voice, dsp/arch/libdragon_mixer.c)
```

`mkBakedInstrument` also gates for **silence, over-quiet renders (< −40 dBFS),
and clipping**. These are defects you would otherwise only find by ear after
they were already in a ROM — the clipping gate immediately caught that `pm.ks`
runs hot enough that `gain = 0.8` clamped 15% of samples.

The offline renderer (`dsp/arch/offline_ref.c`) also emits the un-encoded
full-quality WAV as `share/<name>-reference.wav`. That is the golden A/B
reference for report Stage 3, if an inner loop is ever hand-ported to RSP
fixed point.

## Not yet built

- **RSP microcode (report Stage 3)** and the **SSH host-proxy (Stage 4)** are
  out of scope by design. M4's budget numbers are the evidence for whether
  Stage 3 is needed at all. (Note Tiny3D ships its own RSPL microcode, and its
  `.rspl` sources are the model to study if Stage 3 ever happens.)
- **The engine runtime has no asset loading yet** (no `m64_asset`/`m64_object`
  layer, no StreamDB wiring). The asset *pipeline* is built (`nix/assets.nix`,
  verified by `examples/assets-demo`), but nothing in `libm64` calls it — a
  ROM that wants a model loads it by hand with `t3d_model_load`, as
  `examples/assets-demo/main.c` does.
- **Screenshot verification is not part of `nix flake check`** — it needs a
  live Wayland session, which the build sandbox does not have. It is a `./dev`
  command, run on a desktop.
- **Hardware deploy is unverified against a physical cart** — `sc64deployer`
  builds and runs (`sc64deployer list` → "No SC64 devices found"), but no cart
  was attached during development, so `./dev deploy`, `./dev debug`, and the
  udev module are untested on real hardware.
- Normalisation is not offered for baked instruments; you set `gain` explicitly
  and the clipping gate tells you when it is wrong.

## Conventions

Every file carries an `SPDX-License-Identifier`. Flake glue and `./dev` are
MPL-2.0, matching DeMoD's framework tier. libdragon (Unlicense) and SummerCart64
(GPLv3) are packaged as separate programs. Faust `.dsp` sources from
`~/Documents/DeMoD/apps/terminus/patches/` are **PolyForm Shield 1.0.0,
non-commercial** — reference them, do not vendor them into an MPL tree.

When the report and a sibling repo's docs disagree, the sibling repo wins for
what the code does today; the report wins for N64 targeting. The report's M64
facts are as of the July 2026 launch window and ModRetro ships OTA updates.
