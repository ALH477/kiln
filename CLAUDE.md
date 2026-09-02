# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Agents: read `AGENTS.md` first. This file is the long form.

## What this is

**Kiln** — a Nix build system for Nintendo 64 / ModRetro M64 software, with a
Faust DSP bridge.

**The project was called M64 until it was renamed.** Three collisions, none
cosmetic: `M64` is ModRetro's shipping FPGA console (this repo used to extrude
the letters as a 3D wordmark, and `examples/interceptor-demo` still prints
*"not sponsored or endorsed by ModRetro"* on screen); `m64p_*` / `.m64` is
mupen64plus' plugin API and movie format, i.e. the dominant N64 emulator, on
the same PC platform Kiln now targets; and it named one hardware target out of
four (NUS-001, the M64, the ED64 Plus, and now the host). Everything is
`kiln_*` / `Kiln*` / `KILN_*`, the archive is `libkiln.a`, the prefix header is
`kiln.mk`, and `nix/checks/kiln-names.nix` fails the build if the old name
comes back. The ModRetro console is still called the M64 and keeps a small
allowlist in `nix/checks/kiln-names-allow.txt` — read its header before adding
to it. The engine's audio layer originated from a feasibility investigation
into running Faust DSP on the N64 for a specific game (SSHitunneller!, ported
as PetaByte Madness) — that game and its design report now live in their own
repository, but the numeric constraints that investigation surfaced (no libm,
no double-precision on the VR4300, per-voice cycle budgets) are still exactly
what the gates below enforce.

The central design idea: those constraints are easy to violate silently and
expensive to discover on hardware, so the build system enforces them rather
than documenting them. A `.dsp` that calls libm, or a voice that emits a
double-precision instruction, fails `nix build`.

Sibling repos under `~/Documents/`, all separate checkouts (not submodules):
`DeMoD` (the Faust corpus, Quanta, `dm.dcf`), `ssh-dungeon-rpg` (SSHitunneller!
itself — Lua game + Rust `russh` transport), `HydraMesh` (the certified
`DeModFrame`/DCF-Audio/SuperPack wire), `PetaByte-Madness` (the flagship game
this engine was built for) and `ganja-goblin` (a second, unrelated game) —
both split out of this repo so the engine here could be published and
MIT-licensed independently of either game's content.

## Commands

```bash
nix develop            # toolchain + libdragon + faust + ares; sets N64_INST
nix build .#hello      # -> result/hello.z64
nix run .#ares -- result/hello.z64
nix build .#engine-demo  # 3D + 2D GUI worked example
nix flake check        # the pre-push gate — see "The gates" below
./dev shot engine-demo out.png   # boot in Ares on Hyprland, capture + pixel stats
./dev pc               # RUN a game natively — window, pad, sound, no emulator
./dev web --serve      # the same game in a browser (wasm32), on :8080
./dev arch riscv64     # the host gates on another architecture, under qemu
./dev doctor           # toolchain / libdragon / cart status
./dev deploy hello     # sc64deployer upload to a SummerCart64
./dev debug            # debugf() stdio over USB
```

`./dev` has more than the above and the rest is easy to miss. **Read
`./dev help`**, and note in particular:

```bash
./dev drive <rom> <script.txt> [outdir]   # boot with a uinput virtual pad and
                                          #   run an input script (wait/press/
                                          #   hold/stick/shot). tools/drive/*.txt
./dev rec <rom> [out.mp4] [dur]           # record a clip, Ares-only audio
./dev inspect <rom>                       # Ares + GDB server, prints the attach line
./dev mapmaker                            # three.js .map editor on :8000
./dev poser / poser-stage / poser-verify   # three.js animation editor on :8001
./dev map-validate <file.map>              # round-trip through quake_map.py
./dev forge-push <file.map> [card]         # put an existing level on the
                                           #   flashcart's SD card to EDIT it
./dev forge-pull [card] [name]             # bring a console session back into
                                           #   assets/ + run the validator
```

**To look at one screen or mode of a game, build a jump ROM rather than
driving to it.** `nix build .#forge-cam` boots Forge straight into CAM mode,
for example; `./dev shot forge-cam out.png 6` then captures it with **no
controller involved**, which matters because `./dev drive`'s uinput→SDL→ares
binding chain is fragile (see `tools/n64-drive.sh`'s header) and `./dev shot` has
no input path at all by design. See the **`n64-verify`** skill for the whole
verification ladder — it is the single most useful thing to read before
debugging anything visual.

Inside `nix develop`, a libdragon project builds with a **plain `make`** —
`N64_INST` and `N64_GCCPREFIX` are already exported. Verified working.

## Skills

Four skills in `.claude/skills/` carry the operational detail this file only
summarises. Load the relevant one rather than rediscovering it:

- **`n64-modeling`** — authoring geometry: units, vertex colours, materials and
  TMEM, what the runtime can collide with, lighting and fog, camera framing.
- **`n64-animation`** — rigs, rigid skinning's one-bone-per-vertex hardware
  limit, the rig-JSON interchange, the three-layer convention proof, and the
  poser.
- **`n64-verify`** — booting, capturing, the jump ROMs, the debug overlay and
  `kiln_debugdraw`'s spatial layers, and how to read pixel statistics.
- **`n64-forge`** — authoring levels ON the console: the voxel editor, its two
  reductions, the ED64 Plus SD-card loop, and where a test belongs.

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
nix/engine.nix      libkiln — the Kiln engine (3D on Tiny3D, 2D GUI on rdpq,
                    audio on libdragon's RSP mixer, actors/rooms/camera/
                    skeletal animation — see "Phase B" below).
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
nix/faust.nix       mkFaustVoice / mkBakedInstrument, plus the gates. The cycle
                    budget gate is a HARD failure when frame-scoped weighted
                    cycles exceed the declared budget (report §4). Baked
                    instruments export their sample rate via
                    `nix-support/audio-rate` for mkN64Rom cross-checking.
                    dsp/arch/           Faust architecture files, written from scratch against
                    Faust's C ABI (libdragon_mixer.c = live, offline_ref.c =
                    full-quality host render). The live architecture supports
                    accumulation mode (`_render(out, n, accumulate)`) for
                    summing multiple voices and per-voice gain (`_set_gain`).
nix/host-math.nix   libdragon's OWN fast-math library, compiled NATIVELY.
                    The first brick of the host target. fgeom.h is 661 lines of
                    plain C, so the host gets the real fm_vec3_t and the real
                    fm_sinf — not a hand-copy. One patch: four MIPS
                    instructions become the libm calls libdragon documents them
                    as optimising, held true by nix/checks/kiln-hostmath.nix.
plat/host/src/      the host 2D AND 3D passes: a SOFTWARE rasteriser (fill rect,
                    gouraud triangle, the builtin font) behind the rdpq and
                    display surface, plus a deterministic PNG writer. Software
                    and not OpenGL on purpose — the gate has to run in the Nix
                    sandbox, and this pass draws rectangles. It also counts
                    what it drew (kiln_host_counters), because pixels-written
                    is the quantity the console actually spends. host_t3d.c
                    reimplements the Tiny3D API's semantics — vertex cache,
                    matrix stack, lights, fog, depth — and HONOURS the s16.16
                    matrix quantisation rather than staying in floats, because
                    a host that is more precise than the console disagrees
                    with it about exactly what precision decides. Every RSP
                    limit that is silent on hardware (the 70-vertex cache, the
                    matrix stack) is an assert. host_panic.c is the one place
                    plat/host implements a kiln_* function, because kiln_panic
                    is a CPU exception handler and has no shared logic to
                    duplicate — on the host it is a SIGSEGV handler with a
                    backtrace. host_tex.c is TMEM: 4 KB, tracked, and asserted,
                    because overflowing it on console does not fail — it wraps,
                    and you get a texture built out of whatever else was
                    resident, with no diagnostic at all. It is one of the very
                    few console limits a host build can genuinely check.
                    host_t3dmodel.c is a .t3dm READER, and the only part of the
                    backend that reimplements a file format rather than an API
                    — Tiny3D's loader relocates the file in place, which a
                    64-bit host cannot do. See the hard-won facts for the three
                    things that cost real time there.
plat/host/src/       host_io.c is DragonFS over a real directory, the joypad,
                    eepromfs over one file with the console's 4/16 Kbit sizes
                    ENFORCED, sprite parsing, and libcart reporting no cart.
                    host_audio.c is the mixer's BOOKKEEPING and explicitly no
                    samples: nearly all of kiln_audio is channel arithmetic —
                    a 32-channel budget partitioned into SFX and music ranges,
                    priority voice stealing, room crossfades — and none of it
                    needs PCM to be checkable. mixer_poll says "SILENCE" once
                    rather than pretending.
plat/host/include/  the host's <libdragon.h> and <t3d/t3dmath.h>. The shim sits
                    at the libdragon/Tiny3D API boundary, NOT at a new
                    engine-internal HAL, so no engine .c changes and no #ifdef
                    is added anywhere. Read the headers before adding to them:
                    the rule is copy the real definition, never approximate it.
plat/host/src/      host_wav64.c is .wav64 → PCM: the 28-byte header, the
                    codebook, the block-planar stereo layout, and the Huffman
                    layer audioconv64 wraps VADPCM in by default. The ADPCM
                    itself is decoded by libdragon's OWN vendored codec
                    (MPL-2.0, compiled by nix/host.nix from the pinned input,
                    never copied into this tree) — the host decodes with the
                    decoder that belongs to the encoder that made the files.
nix/host.nix        the host tier, built ONCE, for any toolchain. Owns the
                    compile recipe seven checks used to each carry a copy of,
                    and the four targets: native, wasm32 (emcc/node),
                    aarch64 and riscv64 (musl, static, qemu-user). Each
                    target carries its own hostMath, zlib and VADPCM build,
                    a libkilnhost.a and a libkiln.a driven off
                    engine/modules.mk's HOST_MODULES, and mkCheck/mkProgram/
                    mkGame. -ffp-contract=off lives here and is load-bearing:
                    see "One renderer, four architectures" below.
plat/shell/         the launcher. shell_sdl.c is the native window,
                    shell_web.c is the browser (canvas + Gamepad API + Web
                    Audio via EM_JS), kiln_shell_common.c is everything they
                    must agree about — argv, the key map, the stick shape,
                    the frame deadline. It does NOT rasterise: plat/host
                    draws the frame and the shell blits it, so the window
                    shows the pixels the gates compare.
tools/n64-shot.sh   boot a ROM in Ares on Hyprland and grim its window.
```

Two prefixes, deliberately distinct: **`N64_GCCPREFIX`** is the toolchain,
**`N64_INST`** is the library prefix. `n64.mk`'s `N64_GCCPREFIX ?= $(N64_INST)`
override exists for exactly this case, so the compiler never has to be merged
into the library tree. `N64_INST` itself *is* a symlinkJoin, because libdragon,
Tiny3D and libkiln all expect to be installed into one prefix (Tiny3D's
`t3d-inst.mk` literally does `-lt3d`, which only resolves if `libt3d.a` sits
beside `libdragon.a`) and Nix store paths are immutable.

## The engine — the whole inventory

**53 modules plus one header-only, one flat directory** (`engine/src/kiln/`), 1:1 `.h`/`.c`, ~13,000
lines, ~290 public `kiln_*` functions. The sections below this one describe
Phases B/C/D in detail and do NOT cover everything — this table does. Anyone
(or anything) planning against the Phase sections alone will conclude the engine
is about half its actual size.

**`engine/modules.mk` has the ONE `MODULES` list**, and nothing else may hold a
copy of it. `nix/engine.nix`'s installCheck reads it via `make print-modules`
rather than restating it; adding a module is one edit. It moved out of
`engine/Makefile` when a second build of the same sources appeared — the
native/host one — because two Makefiles each carrying the list is exactly the
drift the single list existed to prevent. `print-modules`, `print-headers` and
`print-host-modules` all live there too, so `make -f modules.mk <target>` works
with no `N64_INST` in scope.

There is also a **`HEADER_ONLY`** list, currently just `kiln_camkey`: a module
that is installed and contributes no object. It cannot live in `MODULES`, which
drives `$(OBJS)` and would fail the archive with "No rule to make target". The
installCheck asks for `make print-headers` (MODULES + HEADER_ONLY) so a
header-only module is still verified as installed.

And a **`HOST_MODULES`** list: the 51 modules that compile natively, against
`plat/host/include`'s `<libdragon.h>` and `nix/host-math.nix`. It is a claim,
and `nix/checks/kiln-parity.nix` checks it in **both directions from one run** —
every listed module must compile, every unlisted one must not — so it cannot
drift either way. The list grew as `plat/host/` did — 6 → 20 → 21 → 37 → 40 →
49 → 51 as the math, system, 2D, texture, model, IO and streaming-pacer tiers
landed, and every step was announced by the gate failing rather than noticed
later — `kiln_stream` and `kiln_streamio` (below) both turned out to compile
natively for free, once `kiln_asset`/`kiln_room`/`kiln_tile`/`kiln_cache` all
did, and `kiln-parity` is what caught that rather than leaving them
unlisted. **Two are left out
and both are principled:** `kiln_video` needs an MPEG1 decoder, and
`kiln_panic` is a CPU exception handler — it reads VR4300 register state out of
libdragon's `exception_t`, which has no host analogue, so
`plat/host/src/host_panic.c` provides its two symbols over signals instead.

| cluster | modules |
|---|---|
| frame + scene | `kiln_engine` (frame/scene/lights/fog/`kiln_scene_project`/`kiln_scene_depth`/transforms), `kiln_gui` (rect/panel/text/bar/line) |
| runtime objects (Phase B) | `kiln_actor`, `kiln_room`, `kiln_camera`, `kiln_skel` |
| feel (Phase D) | `kiln_input`, `kiln_clip`, `kiln_dict`, `kiln_map`, `kiln_surface`, `kiln_sound`, `kiln_event`, `kiln_target`, `kiln_player` |
| streaming (Phase C/E/F) | `kiln_asset`, `kiln_scratch`, `kiln_cache`, `kiln_tile`, `kiln_lod`, `kiln_twopass`, `kiln_stream`, `kiln_streamio` |
| first person + shooting | `kiln_fpscam`, `kiln_weapon`, `kiln_weapons`, `kiln_projectile`, `kiln_inventory`, `kiln_trigger`, `kiln_context`, `kiln_dialogue` |
| simulation | `kiln_physics`, `kiln_crater` |
| visual effects | `kiln_texanim` (scroll/flipbook/palette/offscreen), `kiln_vanim` (RSP VFX, morph, deform) |
| party game | `kiln_rng`, `kiln_dice`, `kiln_board`, `kiln_turn`, `kiln_char` |
| screens + UI | `kiln_widget`, `kiln_splash`, `kiln_video` (MPEG1), `kiln_save` (eepromfs) |
| audio | `kiln_audio` |
| debug | `kiln_debugdraw` (lines/AABB/axes/path/text/**frustum**), `kiln_console`, `kiln_panic`, `kiln_prof` |
| authoring (Forge) | `kiln_voxel` (block grid + the two reductions), `kiln_voxmesh` (quads → Tiny3D vertices, runtime CI4 atlas), `kiln_store` (SD / save chip / ROM) |
| camera data | `kiln_camkey` (the keyframe + its Catmull-Rom, **header-only**), `kiln_camlint` (the static validator) |

**`kiln_debugdraw` is new and is the first thing to reach for on anything
spatial.** World-space lines, AABBs, axis gizmos, polylines and labels, drawn in
the 2D pass through `kiln_scene_project` so it cannot perturb the frame it
describes. The engine had none of this: a clip brush, an actor's bounds, a
trigger volume, a camera path and a trace normal were all reachable only as
numbers printed by a HUD, and that gap is directly responsible for two of the
worst bugs this project has had. See the `n64-verify` skill.

### What genuinely does not exist

Worth stating, because absence is invisible and each of these gets proposed:

- **No frustum culling anywhere.** `kiln_twopass.h:63` says so outright, and
  `kiln_actor_draw_all` / `kiln_room_draw_all` draw everything unconditionally.
  Tiny3D's `t3d_model_bvh_query_frustum` could do it — but every PetaByte
  Madness model is built `bvh = false`, so it is unavailable by construction
  there. `kiln_lod_visible` is a distance cutoff, not a frustum test.
- **No occlusion culling, PVS or portals.** `kiln_map.h`: "No BSP / PVS /
  portals." Room streaming is residency, not visibility.
- **No particle system**, no billboard/sprite-in-3D helper.
- **No scene graph and no bone sockets.** `KilnTransform` has no parent pointer,
  and there is no way to hang a weapon off a hand bone through the engine
  despite `kiln_skel` holding a full `T3DSkeleton`.
- **No material / texture / light API.** Lighting is four fields on `KilnScene`
  (max 4 of Tiny3D's 7 directional lights, no point or spot, no shadows).
  "Material" exists only as `KilnSurfaceDef` (gameplay: friction + footstep SFX;
  its `render_flags` byte is declared and never read) and `KilnTexAnim`.
  `kiln_map` parses no texture or UV data at all.
- **`kiln_prof` is CPU-only** — six COP0-TICKS zones. No RSP/RDP timing, no
  triangle or draw-call counters.
- **No viewmodel** (first-person weapon mesh), and `kiln_dialogue` does no text
  wrapping — both documented as deliberate.

## The engine's two layers (engine/, examples/engine)

Two layers, one state transition per frame:

```
kiln_frame_begin()          attach framebuffer + Z-buffer
  kiln_scene_begin(&scene)  3D: Tiny3D, perspective, lit, depth-tested
    ... draw geometry ...
  kiln_gui_begin()          <- the seam: depth OFF, standard combiner
    ... panels/text/bars ...  2D: rdpq, screen-space
  kiln_gui_end()
kiln_frame_end()            present
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

## Phase B — actors, rooms, camera, skeletal animation

The "runtime" half of the engine, on top of the 3D/GUI layer above: live
game objects (`kiln_actor.h`), streamed world geometry (`kiln_room.h`), an
OoT-style follow camera (`kiln_camera.h`), and skeletal animation
(`kiln_skel.h`). Verified by `examples/actors-demo`, `examples/rooms-demo`,
and `examples/camera-skel-demo` (camera + skel + `kiln_audio` together).
Audio itself is documented separately below; the connective tissue between
Phase B and audio is `kiln_room_current()` feeding
`kiln_audio_set_room_music`/`update_rooms` (see "The audio layer").

Each module is modelled on a specific piece of Ocarina of Time's engine, and
each header's own comment says so and explains what was deliberately left
out. The summary, so it's in one place:

- **`kiln_actor.h`** — OoT's Actor/ActorProfile split: a flat, caller-owned
  pool (not malloc-per-actor), category-ordered update/draw lists (player
  before enemies before props, same reasoning OoT gets predictable draw
  order and cheap category queries from), one fixed-size inline state block
  per instance sized to the largest actor type (`KILN_ACTOR_STATE_MAX`,
  override before including the header if 64 bytes is too small — this is
  OoT's "instance struct sized to the overlay's max" idea), and
  handle+generation instead of raw pointers so a stale reference resolves to
  NULL instead of a reused slot. **Not carried over:** OoT's overlay/segment
  paging — actor code is never paged in and out of a small ROM window here,
  every actor type's code is resident for the whole game, which is the
  simpler and correct choice until a game's actor code genuinely doesn't fit
  in RAM at once. No collision/physics system of any kind yet (see "Not yet
  built").
- **`kiln_room.h`** — OoT's cross-shaped loaded-room set: AABB-overlap with
  the camera (not a radius, which would also pull in both diagonal
  neighbours at every corner) plus each room's `neighbours[]` table as
  streaming candidates, actor spawn templates deferred to room-load time so
  an actor is never resident in RAM without a mesh to occlude it against.
  **Not carried over:** no BG collision mesh streamed per room — there is no
  collision system at all yet, so `user_mesh` is drawn but never collided
  against.
- **`kiln_camera.h`** — OoT's follow-camera boom: a fixed-length spring arm
  behind the target whose *heading* lags the target's facing direction on
  its own damper, separate from the eye position's damper, so a sharp turn
  swings the camera around over several frames instead of snapping it (see
  `Camera_Normal1` in the OoT decomp for the shape of this). Phase 5 grew
  the two things Phase B deliberately left out: a **mode stack**
  (`kiln_camera_push`/`pop`, `KILN_CAM_NORMAL`/`TARGETING`/`CUTSCENE`,
  fixed 4-deep, restores mode + smoothed state together) and a
  **collision-aware boom** (`kiln_camera_set_collision` opt-in; the boom is
  `kiln_clip_ray`'d against the world each frame and pulled in on a hit).
  Both are opt-in and default-OFF so Phase B examples link and behave
  unchanged. Also a deliberate engine-wide departure: damping is
  linear-per-frame (`t = min(1, speed*dt)`), not `expf(-t)` — same
  qualitative curve for one multiply instead of a transcendental call,
  consistent with this engine's "single precision, no gratuitous libm"
  stance (see "Constraints that shape everything").
- **`kiln_skel.h`** — not modelled on OoT (which predates glTF-style skinning
  entirely) but on Tiny3D's own animation idiom
  (`t3d/t3dskeleton.h`, `t3d/t3danim.h`, demonstrated in Tiny3D's
  `examples/08_animation`): one primary skeleton with fixed-point bone
  matrices, one pose-only clone blended into it by a live scalar factor —
  the same idea as an idle/walk locomotion blend, driven here by
  `examples/camera-skel-demo`'s movement speed. **Not carried over from a
  "full" animation system:** no N-way blend tree or partial-bone masks, two
  slots only. **A hardware constraint, not a choice:** skinning is rigid,
  one bone per vertex — `gltf_to_t3d` reads only the first `JOINTS_0`
  channel per vertex (see
  `tools/gltf_importer/src/parser.cpp` in the Tiny3D source), there is no
  4-bone weighted blend on console. `tools/gen_skel_gltf.py` generates the
  hand-authored 2-bone test rig `examples/camera-skel-demo` uses — a glTF
  skin needs no fast64 export and no `inverseBindMatrices` accessor
  (`gltf_to_t3d` derives inverse bind poses itself from the joint nodes'
  own TRS hierarchy), just `skins[].joints` naming a node chain and
  `JOINTS_0`/`WEIGHTS_0` mesh attributes; that generator is the reference
  for what a hand-built (non-Blender) skinned test asset needs to contain.

Verified: `nix build .#camera-skel-demo` links clean (300 KB text, matching
the other actor-system demos' size class) and passes the `audioRate = 32000`
check. `./dev shot camera-skel-demo` was attempted but the capture is not
trustworthy in this sandbox — Ares maps a window (correct geometry, correct
PID, all of `tools/n64-shot.sh`'s own sanity checks pass) but its surface
never actually composites here, so the "capture" is whatever desktop window
sits behind it rather than the emulator. That is a property of this
particular desktop/Vulkan environment, not of the ROM or the script; treat
`nix build` + the gates as the verification for this ROM until it's run on
a desktop where Ares' Vulkan surface actually presents.

## Phase C — runtime asset streaming (engine/src/kiln/kiln_asset.*, examples/streamdb-demo)

`kiln_asset.h` is the runtime half of a gap CLAUDE.md itself used to describe:
`nix/assets.nix`'s build-side pipeline (`mkModel`/`mkSprite`/...) already
produced converted assets, but nothing in `libkiln` opened a StreamDB
container at runtime — every ROM that wanted an asset loaded it by hand
with `t3d_model_load`/`sprite_load` against a DFS path. `kiln_asset` closes
that gap for the two things worth indexing rather than just listing as
loose files.

**Two containers, two purposes, on purpose:**
- **DFS** — a flat directory of loose files baked into the ROM, for
  anything an engine loader is hardcoded to open by `rom:/...` path
  (`t3d_model_load`, `wav64_open`). `examples/assets-demo`,
  `examples/audio` and everything under "Geometry authoring" above use
  this — it is also all `mkBlenderModel`'s output is meant for.
- **StreamDB** — one CRC-checked, suffix-indexed container
  (`streamdb-embedded/`, a bare-metal reimplementation of the upstream v3
  format sized for 4 MB RDRAM rather than upstream's pthreads/flock/fsync
  host implementation) read straight out of ROM, for content that
  benefits from being indexed rather than named: level layouts, dialogue
  tables, actor params, and — via `kiln_asset_model`/`kiln_asset_sprite` —
  any model or sprite a game wants to find by suffix scan ("every `.t3dm`
  in this DB") instead of a hardcoded path per asset.

**One arena, caller-owned, no cache.** `kiln_asset_open` takes a caller-sized
arena (`kiln_asset_probe_size` sizes it); a heap failure mid-level is not
recoverable on this console, so sizing happens at boot, not lazily.
`kiln_asset_model`/`kiln_asset_sprite` malloc and return — the caller holds
the pointer, same contract as `t3d_model_load`. No cache table; a bounded
one is a same-day follow-up if an actor type ever needs on-demand loading,
not needed by anything built so far.

**`kiln_asset_model` needs `t3d_model_load_buf`,** which Tiny3D upstream does
not expose (`t3d_model_load(path)` only) — `nix/patches/tiny3d-load-buf.patch`
adds it as a pure refactor (extracts `t3d_model_load`'s body into
`t3d_model_load_buf(buf, sz)`) so a `.t3dm` already read out of a StreamDB
payload can be parsed without a round-trip through DFS. `nix flake check`
fails loudly here, not silently at runtime, if a Tiny3D bump ever makes the
patch stop applying.

**`kiln_asset_wav64` is deliberately NOT provided** — see "Not yet built".

### Datafiles: StreamDB vs loose DFS — the actual decision framework

Kiln's answer to "the engine consumes data files, the way id Tech 4 does" is
StreamDB — it is this platform's closest analogue to a `.pk4` pak, and it is
what `kiln_stream`/`kiln_streamio` (Phase F, below) pace loads through. But
"always use StreamDB" is false today, for a concrete, structural reason, not
a style preference:

- **Use StreamDB** for anything indexed/keyed, suffix-searchable, or where
  bundling many small records (models, sprites, level layouts, dialogue,
  actor params) into one CRC-checked container beats N loose DFS entries.
  `nix/assets.nix`'s `mkAssetPak` makes this the *easy* default for a new
  ROM: hand it a list of already-built asset derivations and it packs every
  file each one produced under its own `filesystem/`, keyed by that file's
  own relative path — the key can't drift from the file the way
  `mkStreamdb`'s hand-typed `entries` can (see the hard-won fact on an asset
  builder's `name` being the filename).
- **Use loose DFS** for anything an engine loader only knows how to open by
  hardcoded path with no in-memory variant. Audio is the standing example —
  `wav64_open` has no buffer form, so `kiln_asset_wav64` cannot exist until
  libdragon grows `wav64_open_buf` — but it is not the only one: an
  **animated** model isn't actually single-file either. Tiny3D's
  `t3danim.c` streams clip data from sidecar `.N.sdata` files via
  `asset_fopen(animDef->filePath, ...)`, a hardcoded DFS-path open, same as
  `wav64_open`. `examples/camera-skel-demo`'s rig stays on loose DFS
  entirely for exactly this reason — packing its `.sdata` sidecars into a
  StreamDB would build fine and fail at runtime, because Tiny3D would still
  try to open them by DFS path regardless of where the `.t3dm` itself came
  from.
- **Realistic scope on a fixed ROM cartridge:** there is no writable general
  filesystem at runtime (SD via `kiln_store` is real, but it is Forge's
  editor-save path, not a game-content mod directory) — so "id-Tech-4-style"
  here means "one build-time-baked pak instead of scattered hardcoded DFS
  paths," not runtime `fs_game`-style directory swapping.

The realistic shape of most real content is therefore a **mix**: model and
sprite through StreamDB, audio (and any animated model) through loose DFS in
the same ROM — not an all-StreamDB or all-loose-DFS choice.

Verified by `examples/streamdb-demo` (one `.streamdb` packing a model, a
sprite, and a raw level-layout blob — `kiln_asset_model`'s patched
load-from-buffer path, `kiln_asset_sprite`, `kiln_asset_load` on the raw
blob, and `kiln_asset_find_suffix`), `examples/assets-demo` and
`examples/openworld-demo` (the realistic mixed pattern: `mkAssetPak`-built
StreamDB for models/sprites, loose DFS for the `.wav64`), and
`examples/camera-skel-demo` (the animated-model case that stays loose DFS
entirely, and why).

## Phase D — OoT + id Tech 4 feel (input, clip, dict, map, surface, sound, event, target, player)

The "feel" half of the engine, layered on Phase B: a clean-room
implementation of the primitives that make a game feel like Ocarina of Time
to play *and* like id Tech 4 (Doom 3) to author for. Eight new modules, two
existing modules extended additively. Each module's own header comment
names the id Tech 4 / OoT primitive it models and what was deliberately
left out. Verified by `examples/clip-demo`, `examples/map-demo`,
`examples/event-demo`, and the Phase 6 integration proof
`examples/oot-demo`.

- **`kiln_input.h`** — one-poll-per-frame joypad wrapper. Squared-magnitude
  deadzone (a disc, not a square — a per-axis threshold makes the stick
  report motion on a resting diagonal). Button edges (`edges`/`released`)
  computed by diffing against last frame, so example code stops
  hand-rolling XOR. Replaces direct `joypad_poll` everywhere new.
- **`kiln_clip.h`** — idPhysics / CollisionModel trace analogue. World = a
  flat array of brush AABBs per loaded room (no BSP — overkill for OoT-room
  counts on a 4 MB console). Slab-method swept AABB vs AABB, single
  precision with a 1e-3 epsilon (s16.16 world scale; 1e-4 produces visible
  contact jitter). `kiln_clip_slide` is the iterative clip-and-retry
  SlideMove shape (Doom 3's `idPhysics_Player::SlideMove`) — what makes a
  player slide along a wall instead of stopping dead. No rotation traces,
  no contents test, no contact-point list — those are layered on top by a
  game that needs them.
- **`kiln_dict.h`** — idDict analogue. Module-global interned key table
  (256 caps, one boot allocation); per-instance `KilnDict` is a fixed
  16-slot array of `{key_id, type, union{int,float,fm_vec3_t,str_id}}`.
  Embedded in `KilnRoomSpawn` so spawn args ride with the spawn template.
  `kiln_dict_set_auto` parses "0 0 0" as vec3, "5.5" as float, "5" as int,
  else string — the auto-typing idDict's `Set` does on text input.
- **`kiln_map.h`** — idMapFile analogue. Parses the existing Quake `.map`
  text format (`assets/quake_test.map`, `assets/oot_test.map`). One-pass
  tokenizer: entity `{ "k" "v" ... <brushes> }`; brush blocks reduced to
  AABB (componentwise min/max of plane points) + one parallelogram per
  face. Non-axis-aligned faces render as parallelograms, not true polygons
  — flagged as a known limit, fine for rectangular OoT-style rooms.
  `classname` → `profile_id` via `kiln_map_register_classname`. One `.map`
  = one room for the demo; multi-room games load several `.map` files and
  connect them via `target_room` epairs later.
- **`kiln_surface.h`** — surface-prop table analogue. `KilnSurfaceDef[256]`
  of `{ friction, footstep_sfx, render_flags }`, indexed by
  `KilnTrace.hitsurface`. A real Doom 3 binds materials to textures with
  surface flags (metal, flesh, stone); on an N64 with no programmable
  pixel pipeline the "material" layer is one small fixed table the gameplay
  code reads, separate from the rdpq combiner the renderer uses.
- **`kiln_sound.h`** — sound-shader analogue, separate from `kiln_audio.h`
  for clarity. `KilnSoundShader { name, wav64_path, base_vol,
  falloff_radius, loop }`; `kiln_sound_play(name, world_pos, pitch)`
  computes distance→volume and listener-facing→pan (stereo only — no HRTF
  on a 93.75 MHz VR4300) and triggers `kiln_sfx_play_ex`. One
  `kiln_sound_update_listener` per frame; looping positional shaders
  (torches, machines) recompute vol/pan from it.
- **`kiln_event.h`** — idEvent analogue. One flat pool of 256 slots (~7 KB)
  and a single `kiln_event_process` per frame — per-actor queues would mean
  per-actor malloc, which the engine deliberately never does (see
  `kiln_actor.h`'s flat-pool rationale). `kiln_event_post(handle, event_id,
  delay_ms, args, argc)`; `kiln_event_process(dt)` runs BEFORE
  `kiln_actor_update_all` so events land before the actor's own update.
  Pool-full policy: a new event with priority higher than the
  lowest-priority queued event evicts that one (debugf'd); otherwise the
  new event is dropped (debugf'd). Stale targets (despawned before fire)
  are dropped silently — a queued "play idle" event for a killed actor is
  not a warning worth spoiling real bugs with. Dispatched via
  `KilnActorEventFn` on `KilnActorProfile` (NULL = ignore).
- **`kiln_target.h`** — Z-targeting. Cone + range query over the ENEMY and
  NPC category lists (reuses `kiln_actor_first/next` — no spatial index, no
  kd-tree; at OoT enemy counts per room the linear walk is cheaper than
  maintaining a structure). `kiln_target_acquire` picks the smallest-angle
  candidate in the forward cone; `kiln_target_switch` cycles by stick
  direction; `kiln_target_draw_reticle` projects the locked actor's world
  position through the scene's view basis and draws four corner brackets
  via `kiln_gui`, clamping to the nearer screen edge when the target is
  behind the camera.
- **`kiln_player.h`** — the player locomotion state machine. A helper, not
  an actor profile: the player IS an actor (category PLAYER), and its
  profile update/draw call into `kiln_player_*` which owns the
  IDLE/WALK/RUN/ROLL/ATTACK/JUMP/FALL machine. Reads `kiln_input_get(port)`,
  integrates velocity against `kiln_clip_slide`, probes ground with
  `kiln_clip_ground`, and posts `KILN_EV_PLAYER_FOOTSTEP` events at a
  cadence proportional to speed — the actor's `KilnActorEventFn` dispatches
  to `kiln_sound_play` keyed by the underfoot surface. Camera-relative
  movement basis set each frame via `kiln_player_set_camera_basis`.

**Existing modules extended (additively, backward-compatible):**
- **`kiln_actor.h`** — `KilnActorProfile` gained `KilnActorEventFn event` and
  `kiln_actor_dispatch_event` (used by `kiln_event_process`). `kiln_actor_spawn`
  takes a `const KilnDict *dict` (may be NULL) the profile's `init` reads
  spawn args from. Old examples pass NULL and behave as before.
- **`kiln_camera.h`** — mode stack + collision-aware boom (see Phase B
  bullet above). Both default-OFF; Phase B examples link and behave
  unchanged.

**`nix/rom.nix` asset-merge fix:** the per-asset `cp -rL` preserved Nix
store dir mode 0555, so two `mkSound` outputs sharing a `sfx/` subdirectory
collided with "Permission denied". Fixed with `cp -rL
--no-preserve=mode` plus an in-loop `chmod -R u+w` so a second asset can
write into a subdir created by the first. Affects every multi-asset ROM
that ships two assets in the same subdir; `clip-demo` (`[ demoSound
stepSound ]`) was the first to hit it.

Verified by `examples/oot-demo`: a player actor walks `assets/oot_test.map`
(sliding via `kiln_clip`), Z-targets two orbiting enemies (camera pushes
`KILN_CAM_TARGETING`, reticle projects through the scene), emits footstep
SFX via `kiln_event` + `kiln_sound`, and gets a 1.5 s `KILN_CAM_CUTSCENE`
pan on boot that pops back to NORMAL — every Phase D module in one frame.
`nix build .#oot-demo` and `nix flake check` are green (32 checks).

## Phase F — a priority/budget pacer for room+tile streaming (kiln_stream, kiln_streamio)

Closes a gap Phase C/E left open: `kiln_room`, `kiln_tile`, `kiln_asset` and
`kiln_cache` were four independent systems — no example ever wired real asset
loading into room/tile streaming, `kiln_room` had **no per-frame load budget
at all** (a whole cross-shaped set can load synchronously in one frame), and
`kiln_tile`'s only budget (`load_budget`) was a flat count serviced in
slot-scan order, not by distance or urgency. This is **not** an async I/O
system — nothing in this engine or platform does background/threaded I/O,
and every `kiln_asset_model`/`kiln_asset_sprite` call stays a single
blocking call. What it adds is a **pacer**: which of the currently
outstanding requests gets that blocking call issued *this frame*, in
priority order, under a real byte/count budget, deferring the rest exactly
the way `kiln_tile`'s existing `TILE_PENDING` already did for its narrower,
FIFO-only case.

- **`kiln_stream.h/.c`** — pure admission policy. A flat pool
  (`KILN_STREAM_MAX_PENDING`, default 64) of `(key, urgency, rank, byte_cost,
  tag)` requests, generation-counted handles packed like `KilnCacheHandle`
  (index biased +1 so slot 0/gen 0 can't collide with `INVALID == 0` — the
  exact bug `kiln_cache`'s handle packing once had, not reintroduced here).
  Pool-full eviction is deliberately `kiln_event_post`'s exact rule (evict
  the lowest-priority PENDING slot only if the newcomer strictly outranks
  it, else drop + `debugf`) — a scarce flat pool under contention is the
  same problem there and here. `kiln_stream_frame_begin` admits the
  highest-`(urgency, rank)` PENDING requests that fit `max_bytes_per_frame`
  and `max_admits_per_frame`, stopping at the first one that doesn't fit
  rather than bin-packing smaller lower-priority ones around it — except a
  single request bigger than the whole per-frame budget is forced through
  when nothing else has been admitted yet, or it would starve forever. Knows
  nothing about `KilnAsset`/`KilnCache`/`KilnRoom`/`KilnTileSlot` — same
  pure-logic-vs-console-glue split as `kiln_voxel`/`kiln_voxmesh` — so it
  sits in `HOST_MODULES` and is asserted on by `nix/checks/kiln-logic.nix`.
- **`kiln_streamio.h/.c`** — binds one `KilnStream` to a real `KilnAsset*` +
  `KilnCache*`, and provides `kiln_streamio_room_on_load`/`_on_unload` and
  `kiln_streamio_tile_on_load`/`_on_unload` — **literal**
  `KilnRoomLoadFn`/`UnloadFn` and `KilnTileLoadFn`/`UnloadFn` implementations,
  not a new callback contract, so a game opts in by pointing its existing
  `kiln_room_system_init`/`kiln_tile_init` function-pointer slots at these
  instead of hand-writing its own. `on_load` does no I/O — it only calls
  `kiln_stream_request` and returns — so `kiln_tile`'s own `load_budget` must
  be set to 255 (its documented "synchronous, load-all-immediately" escape
  hatch) to disable `kiln_tile`'s own throttling; `kiln_stream` becomes the
  sole budget authority. `kiln_streamio_pump`, called once per frame after
  residency update and before draw, issues the real `kiln_cache_acquire`
  (internally `kiln_asset_model`/`kiln_asset_sprite`) for every request
  `kiln_stream` admitted, and writes the result into `room->user_mesh` or
  the tile slot's `user_data` — both already documented as engine-never-
  dereferences, read only by the caller's own null-checking draw callback,
  so writing them asynchronously after the triggering `on_load` returned is
  safe. Turned out to compile natively too (every module it touches already
  does), so it sits in `HOST_MODULES` alongside `kiln_stream` — though its
  own `kiln_asset`/`kiln_cache` round-trip needs real StreamDB content to
  exercise, which is a heavier follow-on check, not required for this to
  land.
- Two health gauges, meant to read 0 in a healthy frame:
  `kiln_stream_dropped_total` (pool full, nothing lower-priority to evict —
  a capacity/tuning problem) and `kiln_streamio_fail_total` (an admitted
  request's asset call returned NULL — a content problem, the same
  "missing map / wrong asset filename" failure class that has already cost
  this project a whole PLAY screen once). Distinct counters on purpose, per
  Forge's "every gauge goes red at the value that means it's lying to you."

Verified by `examples/openworld-demo`, rewired from its original hand-malloc'd
2-vert tile stub onto a real `openworld.streamdb` (`models/tile.t3dm`, one
shared model — the point is the pacer's priority ordering across many
simultaneous requests, not per-tile unique geometry): `nix build
.#openworld-demo` links clean, and `./dev shot openworld-demo` shows the full
5×5 window (25 tiles) loaded through `kiln_stream`→`kiln_streamio`→
`kiln_cache`→`kiln_asset` with `pending 0 dropped 0 failed 0`.

**Getting that screenshot found four pre-existing, previously-unverified
bugs, none of them in the new pacer** — this appears to be the first time
either `examples/streamdb-demo` or `examples/openworld-demo` had actually
been booted rather than just built:
1. `streamdb_emb_io_dfs()` (`streamdb-embedded/src/streamdb_io_dfs.c`) called
   `dfs_open()` with the `"rom:/"`-prefixed path `kiln_asset.h`'s own doc
   comment tells every caller to pass — but `dfs_open` wants the prefix
   stripped, exactly the mismatch `kiln_map_load` already found and fixed
   once (see its comment). Invisible to `nix/checks/kiln-asset.nix` because
   that check builds `kiln_asset.c` against a host stdio stub, never the
   real DFS backend.
2. `examples/streamdb-demo/main.c`'s `kiln_asset_model` call passed a
   hardcoded key length of `17` for `"models/cube.t3dm"`, which is 16 bytes
   — an off-by-one that made the lookup miss every time, never caught
   because nothing exercises `streamdb_emb_find` against real key lengths
   outside a booted ROM.
3. `openworld-demo`'s `kiln_tile_init` passed `NULL` as `user_ctx`, which
   `kiln_lod_selector_cb` dereferences as a `KilnLODConfig*` — every tile's
   distance compare read off a null config, came back "beyond the last
   threshold," and no tile was ever marked resident. (kiln_streamio's own
   `user_ctx` need — `&g_tile_binding` — is what forced this one into the
   open: the demo now reads `lod_cfg` from a module-global directly instead.)
4. `openworld-demo`'s draw callback translated each tile by `tile_center -
   cam_pos` while `kiln_scene_update`'s `t3d_viewport_look_at` already takes
   `cam_pos` as an absolute world position — double-subtracting the camera
   offset and pushing every tile off the far plane. Tiles now draw at their
   actual world coordinates.

All four are content/wiring bugs in example code and a sibling library, not
in `kiln_stream`/`kiln_streamio` themselves, but the project's own precedent
(`kiln-map`, `kiln-parity`, `kiln-logic`'s first runs) is that a check which
has never actually been exercised end to end is a check that might not be
checking anything — this is that lesson recurring one level up, at "has this
ROM ever been booted" rather than "has this gate ever fired."

## The audio layer (engine/src/kiln/kiln_audio.*, examples/audio, examples/live-voice, examples/music)

Three audio paths, all first-class:

```
Baked instruments (report Stage 1, recommended 80-90%):
  .dsp → mkBakedInstrument → host render (-double) → audioconv64 → .wav64
       → mkN64Rom `assets` → DragonFS → kiln_sfx_load/play → RSP mixer

Live Faust voices (report Stage 2):
  .dsp → mkFaustVoice → faust -lang c -single -os → VR4300 object
       → linked into ROM → faust_n64_<name>_render(out, n, accumulate)
       → VR4300 summing into AI buffer alongside mixer_poll

Tracker music:
  .xm/.ym → mkMusic → audioconv64 → .xm64/.ym64
         → mkN64Rom `assets` → DragonFS → kiln_music_load/play → RSP mixer
```

The engine audio layer (`kiln_audio.h`) wraps libdragon's RSP mixer with:
- `kiln_audio_init/update/close` — init, per-frame pump, teardown
- `kiln_sfx_load/play/play_ex/stop` — SFX with priority-based voice stealing
- `kiln_music_load/play/stop/set_volume` — XM64/YM64 tracker music
- `kiln_audio_set_room_music/update_rooms` — room-based music crossfading

Channel partition: `[0..sfx_channels)` for SFX, `[sfx_channels..total)` for
music. Default: 16 SFX + 10 music = 26 channels (max 32).

The cycle budget gate (`nix/faust.nix`) is a **hard failure** when
frame-scoped weighted cycles exceed the declared budget. It is scoped to
the `frame<name>` function only — init/constructor code is excluded. The KS
voice measures 259 weighted cycles in `frame()` (vs 333 for the whole
object, including init).

`mkN64Rom` accepts an `audioRate` parameter that cross-checks baked
instrument rates against the ROM's `audio_init` rate at build time. A
mismatch causes pitch/time drift (not silence), so this catches a subtle
defect class.

The live architecture file (`dsp/arch/libdragon_mixer.c`) supports
accumulation mode: `_render(out, n, accumulate)` saturating-adds to the
buffer when `accumulate != 0`, enabling multiple voices to be summed into
one AI buffer. Per-voice gain is set via `_set_gain`.

## Forge (Forge/) — the level editor that runs on the console

**`nix build .#forge`**, and **`nix build .#forge-selftest` first, on an
unfamiliar cart.** A Minecraft-shaped voxel builder whose save button emits the
content formats this engine already consumes.

Every other authoring tool here runs on the host — `./dev mapmaker` (:8000),
`./dev poser` (:8001), `tools/blender/*` — and all three round-trip through a
browser download and a human moving a file. Forge exists because the judgements
that matter on this hardware cannot be made two hops away: fill rate, whether a
palette still separates once the veil discards hue, whether a corridor reads as
a corridor.

- **The voxel grid is not a style choice.** A greedy-merged run of blocks IS an
  axis-aligned box, which is exactly `KilnBrush`, exactly what
  `tools/mapmaker/src/mapio.js` emits, and exactly what `kiln_map.c` reduces its
  six planes back down to. One algorithm (`kiln_voxel_boxes`), three consumers:
  the clip world, the `.map` export, and WALK mode.
- **Two reductions, and they are NOT the same answer.** `kiln_voxel_boxes` is a
  volume partition (collision, export); `kiln_voxel_quads` is a greedy surface
  extraction over exposed faces only (rendering). Drawing the boxes would draw
  the faces where two boxes meet, and on this hardware wasted fill is the
  expensive mistake. A 480-block room is 3 boxes and 26 quads.
- **The pure half is host-tested, the Tiny3D half is not, and that seam is
  deliberate.** `kiln_voxel` includes only `<stdint.h>` and `<t3d/t3dmath.h>`, so
  it compiles against `nix/checks/stub/` and is asserted on in `kiln-logic`;
  `kiln_voxmesh` includes `<t3d/t3d.h>` and cannot be. The vertex packing was
  split out of the mesher for exactly this reason.
- **`kiln_voxmesh` batches 68 vertices per `t3d_vert_load`** — 17 quads per RSP
  DMA, against `kiln_map_draw`'s one load per 8-vertex face. 68 and not 70
  because a quad is 4 vertices and no quad may straddle a load boundary.
- **The atlas is CI4 by design, not to save space.** 16 tiles of 16×16 in a
  64×64 surface, 2 KB against a 4 KB TMEM (`kiln-voxmesh` asserts that number).
  CI4 + TLUT is the format the veil is built on, so a Forge level is
  veil-capable by construction. The 16-colour palette is also why block types
  cap at 15.
- **…but the atlas is not currently reaching the screen, and every block type
  draws the same grey.** `kiln_voxmesh` puts the block TYPE only in the UVs
  (`:103-106`); vertex colour is `DIR_SHADE[dir]`, greyscale per-face
  brightness, carrying no type at all (`:108`). `forge_geo.c:27` sets
  `T3D_FLAG_TEXTURED`, so the RSP emits texture coordinates — but the
  **combiner** decides whether the texel survives, and it is
  `RDPQ_COMBINER_SHADE` from `kiln_scene_begin` (`kiln_engine.c:126`), which
  outputs vertex colour and discards the texel. Tiny3D's
  `t3d_state_set_drawflags` does not touch the combiner (`t3d.c:300`), and
  nothing in `Forge/src` sets one. So PAINT mode's authored palette never
  appears, and `Z`'s veiled preview cannot change the geometry it is previewing.
  `nix/checks/kiln-voxmesh.nix` renders it both ways and its two committed
  captures are the before and after: ~83,000 texels sampled and thrown away in
  one frame. **The fix is one `rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE)` in
  Forge**, not the engine — `kiln_voxmesh_draw`'s own comment says "Sets NO
  render state: the caller has already chosen the combiner" — and whether the
  palettes still read once it lands is a judgement about a CRT that only
  hardware settles.
- **WALK mode installs the greedy boxes and hands the pad to the real
  `kiln_fpscam`**, so a doorway's width is judged by walking through it. It
  leaves `kiln_clip`'s broadphase **off** on purpose: the grid is 16×16 in XZ with
  Y ignored and its placement pass `assertf`s at 512 brush×cell entries, which a
  floor slab trips, and that assert is a hard crash.
- **Every capacity has a gauge and every gauge goes red at the value that means
  the editor is lying to you.** `chunks n/24`, `quads`, `arena %`, `clip n/512`,
  `cart`/`store`/`bus`. The gauge prints the quad COUNT as well as the arena
  percentage because greedy merging is effective enough that a whole room rounds
  to `arena 0%`, and a gauge reading 0 over plainly-drawn geometry is one nobody
  will believe the next time it reads 0 for a real reason.

### Six modes, in authoring order

`L+R` cycles, and the cycle order is the order you work in. Each has a
`nix build .#forge-<mode>` jump ROM, because `./dev shot` has no input path and a
mode reached only by a chord can only be verified by hand.

| mode | what it authors | out |
|---|---|---|
| GEO | blocks: place, dig, drag-fill, 15 types | the brushes |
| WALK | nothing — you stand in it under real `kiln_fpscam` | — |
| PAINT | the CI4 atlas: 16 tiles of 16×16, 16 colours, `Z` previews the **veiled** palette | `.FRG` |
| ENT | classname + origin + angle + numeric epairs | `.map` point entities |
| LIGHT | key/fill direction and level, ambient, fog range, clear colour | a generated header |
| CAM | keyframes, scrubbing, both curves drawn, validated live | a `PMCamKey` table |

- **PAINT exists because the veil discards hue.** Whether a 16-colour ramp still
  separates once the TLUT swaps is a judgement about a CRT, and no host preview
  settles it — `Z` flips cold/veiled with the geometry still on screen.
- **CAM validates before it saves, using `kiln_camlint`** — the same module
  `./dev cine-lint` runs. A table with a hard failure is refused and logged
  rather than written, because `eye == look` halts the VR4300 inside
  `t3d_viewport_attach`, several layers from the table that caused it.
- **CAM frames its own frustum** on the keys' subject distance (×4) rather than
  inheriting the editor's world-scale far plane, and hides the frustum when the
  viewer is inside it — `kiln_debugdraw.h` says a frustum drawn from inside itself
  is a full-screen X, and at a quarter of the far plane it already is.
- **PAINT and LIGHT do not move the camera.** The D-pad is a texel cursor or a
  light aim, and holding the view still is what makes the judgement possible.

### `kiln_camkey` and `kiln_camlint` moved into the engine when a second consumer arrived

Both were originally local to PetaByte Madness (`pm_camkey.h`, `pm_cine_lint.*`)
and were promoted into the engine proper once Forge needed the same curve
authored, drawn and validated on-console. PetaByte Madness (now its own repo)
keeps thin shim headers over the engine versions — typedefs and defines, not a
second implementation — so its existing keyframe tables compile untouched.

The invariant is the reason: **the curve has exactly one implementation**, flown
by the runtime, drawn by the overlay, measured by the validator and now authored
by the editor. An author tuning against a curve that merely resembles the shipped
one is the same defect as a validator measuring one, and worse, because the
numbers look authoritative.

The move was a rename rather than a rewrite because PetaByte Madness's original
shot struct deliberately *mirrored* its demo-runner's fields instead of taking
one — a coupling refused once, years before there was a second consumer.

### The ED64 Plus loop, which is the point

**libcart is already vendored in the pinned libdragon and names this cart:**
`src/libcart/cart.h:7-12` — `CART_ED` = *"EverDrive-64 V1, V2, V2.5, V3 and
ED64+"*; `debug.h:111-121` lists *"ED64Plus / Super 64"*. `ed_card_wr_dram` is a
real write driver, FatFs is compiled read-write (`ffconf.h:14`), and
`debug_init_sdfs("sd:/", -1)` hangs it off newlib. Nothing in this repo defines
`NDEBUG`, so that surface is live.

So the ROM writes files to the card it booted from, and **the ROM is built once
while levels are data on the card** — editing costs no rebuild and no reflash.
The ROM also emits the `.map` and (as CAM lands) the `PMCamKey` table as plain
ASCII in the dialect already pinned here, so `./dev forge-pull` is a copy plus
`./dev map-validate`, not a bespoke decoder.

**USB is genuinely dead on that cart.** `libdragon/src/usb.c:527` rejects
EverDrive 2.5-class boards outright, so `debugf`, `./dev deploy`, `./dev debug`
and UNFLoader see nothing. Every diagnostic is on screen or in
`FORGE/FORGE.LOG` on the card.

`kiln_store` walks SD → 32 KB save chip → read-only `rom:/`, reports which it
got, and is red on the HUD when it is not writable. Two probe ROMs, because a
gate should fire in both directions: `forge-selftest` must report *no writable
backend* under an emulator, `forge-selftest-sram` must round-trip 8 KB.

**`sram_detect()` returns 0, not −1, when there is no save chip** — its own doc
comment in `sram.h` says −1. A `< 0` test therefore cannot fail, and the SRAM
backend was selected on machines with no chip at all, after which writes went
nowhere and reads came back as zeros, which parse as a valid EMPTY directory.
`try_sram` now requires a positive size *and* round-trips a word through a
scratch area above every slot.

## Geometry authoring (nix/blender.nix, tools/blender/, tools/blender-mcp/)

**Blender authors geometry; it does not author materials.** `nix/blender.nix`
(`mkBlenderModel`) drives Blender headless (`--background`) with a script
from `tools/blender/` that builds a scene via `kilnlib.py`'s helpers
(`make_mesh`/`make_armature`/`make_skinned_mesh`/primitive builders like
`box`/`cylinder`/`uv_sphere`) and exports a glTF. `tools/f3d_inject.py` then
writes `materials[i].extras.f3d_mat` directly — the JSON block
`gltf_to_t3d` actually reads (`tools/gltf_importer/src/parser/
materialParser.cpp`) — from a small preset table (`shade`, `tex0_shade`,
`tex0_alpha`, `prim`), reverse-engineered from that parser rather than
produced by the real Fast64 Blender addon. **Fast64 is deliberately not
vendored**: it is not in nixpkgs, tracks Blender 4.2-4.5 while this flake's
nixpkgs ships a newer Blender, and buys nothing since `gltf_to_t3d` never
talks to the addon anyway — see `nix/blender.nix`'s file comment for the
full reasoning. `tools/blender/goblin.py` is the rigged/animated reference:
one bone per vertex, RIGID skinning only (`make_skinned_mesh`) — same
Tiny3D hardware constraint `kiln_skel.h`'s Phase B notes describe (§ above).

**`tools/blender/quake_map.py` / `godot_scene.py`** extend this pipeline to
two external content formats: Quake `.map` (brush CSG via plane
intersection — every triple of a brush's planes is a candidate vertex,
kept only if it's inside every other plane) and Godot `.tscn` (scene-graph
parsing + placing each node's referenced `.glb`/`.gltf`/`.obj` mesh via
Blender's own importers, transformed through the Godot-Y-up-to-Blender-
Z-up axis conversion). Both scripts follow the same `--out <path>`
convention as `models.py`, so `nix/blender.nix`'s `mkQuakeMapModel` /
`mkGodotSceneModel` reuse `mkBlenderModel`'s exact derivation shape via a
generalised `scriptArgs` parameter — no second pipeline. Both importers'
core geometry/parsing functions are plain Python with no `bpy` import, so
they are unit-testable with a bare `python3 -c` before ever touching
Blender; that discipline caught two real bugs during development (a wrong
three-plane intersection formula, and an inverted face-winding order) by
checking brush output against the canonical 6-plane Quake cube example
before the first headless Blender run.

**`tools/blender-mcp/`** is the interactive front-end: an MCP server
(`server.py`, `FastMCP`) exposing `inspect_quake_map`/`inspect_godot_scene`
(pure Python, no Blender — fast sanity checks) and
`import_quake_map`/`import_godot_scene` (the full Blender → f3d_inject →
optional-`gltf_to_t3d`-preview chain). It runs **outside** `nix build` on
purpose — see the module docstring — because it targets arbitrary,
not-yet-committed content someone is actively iterating on, which is the
opposite of every other pipeline here being hermetic and reproducible.
Once a map/scene is finished, `import_*`'s `nix_snippet` field gives the
exact `mkQuakeMapModel`/`mkGodotSceneModel` call to add after committing
the source under `assets/`, putting it on the same hermetic path as
everything else. `assets/quake_test.map` + the `quakeTestModel` package
are the regression check that path stays working.

## One renderer, four architectures — the host target (`nix/host.nix`, `plat/shell/`)

`plat/host/` was a verification harness: it rendered a frame to a PNG inside a
Nix check and had no window, no clock, no speaker and no architecture other
than the build machine's. It is now also a **playable target**, on
**x86_64, wasm32, aarch64 and riscv64**, and the two facts are the same fact.

**The renderer is the same software rasteriser everywhere, and the window is a
blit.** There is no GL backend and no browser backend. `plat/host/src` draws
the frame; `plat/shell` hands the finished RGBA8888 buffer to an SDL streaming
texture or a canvas `ImageData`. That is a deliberate refusal of the obvious
speed win, and the reason is `nix/checks/kiln-widget.nix`: `tools/uipreview`
once drew its own rectangles and disagreed with the console about panel edge
order, bar inset, and whether `kiln_gui_rect` blends alpha. A launcher that
rasterises is a second implementation of the thing the gates check, and what
is on screen stops being evidence about the ROM.

**Because there is one renderer, there is one set of reference images.** The
same committed PNGs and text manifests are produced byte-identically by all
four architectures — not four blessed references, which would only prove each
architecture agrees with itself. `nix flake check` holds x86_64 and wasm32 to
them; `./dev arch aarch64` / `riscv64` runs three of the same bodies
cross-compiled against musl under qemu-user, and is out of the gate set only
because a cross toolchain is a ~170 MB fetch.

**`-ffp-contract=off` is what makes that true, and it is one flag.** GCC and
Clang both default to `=fast` in GNU C modes, so `a*b + c*d` fuses into an FMA
wherever the target has one. Baseline x86-64 has none, so the references were
stable *by accident*. `host_t3d.c`'s edge function is exactly that shape and
its sign decides whether a pixel is inside a triangle — one fused multiply-add
moves the edge of every triangle on screen. This is the single most likely
thing to silently break the multi-architecture claim, and it lives in
`nix/host.nix` because seven checks each carrying their own compile line is
seven places to forget it and an eighth that never had it.

**musl for the cross pair, not glibc.** Partly because it links static
cleanly for `qemu-user`, but mainly because musl has no `<execinfo.h>`:
`host_panic.c` included it unconditionally, and those two targets are the only
thing in the tree that notices if the guard comes back off.

**No example was edited to make any of this run.** A game is compiled with
`-Dmain=kiln_game_main`, so `examples/<x>/main.c` stays a ROM's `int
main(void)` with its own blocking `for(;;)`. In the browser that loop is legal
because of **ASYNCIFY**, not because control was inverted: the yield goes in
the `vsync` hook, which is called from `display_get` — the exact function the
console blocks in waiting for the VI to release a buffer. The browser yields
where the console waits. `emscripten_set_main_loop` would have meant editing
22 example files into a host-only shape, because `kiln_engine.c` deliberately
owns the frame bracket and not the loop.

**The seam is five function pointers.** `kiln_host.h`'s `KilnHostHooks` —
`present`, `vsync`, `audio_free`, `audio_submit`, `ctx` — all NULL by default,
so a check's pixels and buffer counts are unchanged *by construction* rather
than by remembering to switch something off. `plat/shell` may set those five
and call `kiln_host_pad_set`, and that is its entire licence.

**One thing found by running a game loop that no gate had ever run:**
`audio_can_write()` returned `1`, forever. `kiln_audio_update` is
`while (audio_can_write())`, draining until the device says full, so a device
that is never full never lets the frame end — every host build of a real game
hung on frame one. The host now models a real device's occupancy and credits
it on **presented frames** rather than wall time, because that is the only
clock a deterministic check may have. `nix/checks/kiln-shell.nix` runs the
real `examples/engine/main.c` loop for ninety frames under SDL's dummy drivers
for exactly this reason: one frame proves the linker found everything, ninety
proves the loop comes back round.

**Audio is real now, and one layer of it is a transcription.** `.wav64`
decodes through libdragon's own vendored VADPCM codec (MPL-2.0), compiled from
the pinned flake input by `nix/host.nix` and never copied into this tree — the
host decodes with the decoder belonging to `audioconv64`, which made the
files. The Huffman layer `audioconv64` wraps them in by default is
CPU-side on console, inside a `static` function in a translation unit that
cannot compile natively, so `host_wav64.c` transcribes it and says so at the
point of use. `nix/checks/kiln-wav64.nix` measures **RMS**, not sample count:
a decoder can return the right number of samples full of zeroes and pass every
structural assertion, and silence is the least attributable failure in the
whole audio path.

**What the host still cannot do**, on any architecture: see fill rate (the
console's binding constraint, no host analogue — a PC run is never evidence
that content is affordable); skinned or animated characters (`t3d_skeleton_*`
and `t3d_anim_*` all abort); near-plane clipping (`host_t3d.c` drops a
triangle straddling the eye); `rdpq_sprite_upload` / `rdpq_texture_rectangle`;
and XM/YM tracker playback, which stays bookkeeping. Those are gaps in
`plat/host`, not in the launcher, and each one fails loudly rather than
quietly.

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

**A capture is only reproducible cropped.** Two `./dev shot` runs of the same
deterministic ROM differ in ~37 pixels on the outer rows of the window —
compositor chrome, not emulator output. Crop 12 px on every side and the diff
is exactly zero. A raw `md5sum` of two perfectly good captures will differ and
tell you nothing. The other half of that lesson is on the ROM side: anything
the overlay prints that cannot be the same twice — an uptime, a smoothed frame
rate — has to be suppressed in a build meant to be diffed, if a capture of it
is ever going into a golden-image test.

## Hard-won facts (do not re-derive these)

- **`gltf_to_t3d`'s `--base-scale` defaults to 64, so a model built "at scale
  64" twice renders as a giant clipped edge, not an oversized model.**
  `nix/blender.nix`'s `mkBlenderModel` passes `--base-scale=64` by default —
  Tiny3D stores vertex positions as integers, so a 1-Blender-unit model needs
  this to survive quantisation at all — and `kiln_splash.c`'s runtime
  `KilnTransform.scale` is unrelated: it stays near 1.0 (0.55→1.0 during the
  assemble), because the 64× is already baked into the model's own vertex
  data by the time the ROM sees it. Setting a runtime transform's scale to 64
  "to match baseScale" applies it a second time, and the result is not a
  bigger model — it is a model so large the camera is effectively inside it,
  rendering as one giant diagonal edge across the whole frame. Caught by
  rendering through the host backend and looking, not by calculating: the
  numbers alone give no hint that anything is wrong until you see the frame.
- **A model's authored FRONT, at `kiln_splash.c`'s rest pose, faces −Y, not
  +Y.** The rest pose applies zero extra spin (`ease_out(1.0)` closes the
  turn to nothing), so whichever way a model's detail faces in Blender is the
  way it faces the camera at rest — and it is easy to guess the wrong sign
  when nothing before `tools/blender/kiln_logo.py` had a front/back to get
  backwards. Settled by rendering all four quarter-turns through the host
  backend and looking, not by reasoning about axis conventions.
- **`kiln_map_draw` does not render the brush's faces, and never has.** A Quake
  `.map` gives three points per face, and those points define a **plane** —
  conventionally one unit apart, which is exactly what `assets/quake_test.map`
  uses. `kiln_map.c:155` treats them as face corners and builds the
  parallelogram `p0,p1,p2,p0+p2-p1`, so a 128-unit wall renders as a **1×2-unit
  patch at one corner**, six per brush. `nix/checks/kiln-map.nix` measures it:
  *"face 0 spans 2 units on y; the brush spans 129"*.
  The correct algorithm is already in this repo, on the host side —
  `tools/blender/quake_map.py` intersects every triple of a brush's planes and
  keeps the candidates inside all the others. `kiln_map.c` does no intersection
  at all.
  What DOES work is the AABB, componentwise min/max of the plane points, and
  that is what every consumer actually uses: `kiln_clip_set_world`,
  `kiln_room`'s brush install, PetaByte Madness' PLAY. Which is presumably why
  the rendering was never examined — what you see on screen comes from models,
  and the brushes are collision. The AABB inherits the same off-by-one, so a
  −64..64 brush becomes a −64..**65** collision box.
- **`kiln_map.c:165` truncates a packed normal to eight bits.**
  `uint8_t np = t3d_vert_pack_normal(&n)` takes a `uint16_t` and discards the
  whole x field plus half of y. Three of `quake_test.map`'s six faces come out
  with `normA == 0`. The fix is one word; it is not applied yet because it
  changes what the console shades and wants a look on hardware, and
  `kiln-map`'s reference capture is what will make it visible when it lands.

- **`.t3dm` is three traps and a render will not find them all.** Writing the
  host reader turned up, in order of how quietly they fail:
  **(1) `gltf_to_t3d` emits triangle STRIPS, not indexed triangles.** A cube
  comes out as `numIndices` 0 and `numStripIndices[0]` 24 — six groups of four,
  each group a quad. A reader handling only `t3d_tri_draw` loads every model in
  this repo perfectly and draws nothing.
  **(2) The file is BIG-ENDIAN**, because it is built for MIPS, and Tiny3D
  needs no conversion because the console agrees. Strip index 23 is `0x0017`
  and reads as 5888 little-endian, which trips the vertex-cache assert at once
  — the easy half. The vertex data has the same problem and *no assert can
  catch it*: a position of −32 reads as −8193, so the model renders as a spray
  of triangles that looks like a bad matrix or a broken exporter.
  **(3) `t3d_vert_pack_normal`'s 5.6.5 fields are SIGNED two's complement**
  scaled by 15.5/31.5/15.5 — not an unsigned mapping of [−1,1]. This is the one
  that needed real file data: an unsigned pack paired with its own matching
  unpack is self-consistent, so a hand-built cube lights perfectly and looks
  right, and only a model whose normals were packed by the real encoder shows
  the error. The cube's +Z face carries `0x000f`, which means (0,0,+1) only
  under the signed reading.
  Corollary worth keeping: a self-consistent wrong encoding is invisible to any
  amount of rendering. That is why `kiln-model` runs the real converter instead
  of hand-building its input.

- **A rename is a sed over identifiers and a rebuild over everything else.**
  The M64→Kiln sweep was 9,393 occurrences in 330 files and the mechanical part
  was the easy half. What a `sed` cannot do, and what has to be found by
  looking: `tools/kiln_logo.py` **extrudes the letterforms as geometry**, so the
  wordmark was a content rebuild (K, I, L, N are all straight strokes, which is
  cheaper than the `6` and `4` they replaced); `'M64S'` and `'M6'` are **4- and
  2-byte on-disk magics** whose comments the sweep rewrote while leaving the
  hex, so the comment lied about the constant; and a 4-char test-corpus magic
  `"M64L"` became 5-char `"KilnL"`, which grew a blob from 24 to 25 bytes and
  was **masked by its own `memcmp(..., 4)`** in two places, one of which wrote a
  5-byte magic in front of a `<I` count nothing had got round to parsing yet.
  Grep for length-sensitive literals (`memcmp`, `sizeof("...")`, 3–4 char
  quoted tokens) before believing a rename was cosmetic.
- **The rename gate proved itself immediately, on me.** `git mv` moves the
  INDEX entry, so `git mv` after an unstaged content edit leaves the index
  holding the pre-edit blob at the new path — and a later `git checkout <path>`
  restores that. One engine file silently reverted to `#include "m64_rng.h"`
  and the build failed several steps later. `kiln-names` would have caught it
  in one run.
- **`sram_detect()` aside, on-disk magics deserve a legacy arm.**
  `KILN_STORE_MAGIC` became `'KLNS'` and `KILN_STORE_MAGIC_LEGACY` still
  accepts `'M64S'` on read, in the ROM and in `tools/forge/frg.py` both. A
  project rename must not strand a level already saved to a flashcart; the card
  round-trips forward but not back, which is the direction that matters since
  the ROM is what gets rebuilt.
- **Compiling natively at `-Werror` is a free second opinion, and it collects.**
  `engine/Makefile` sets `-Wno-error` deliberately for third-party header
  noise. The first run of the widened host tier found a dead bounds check:
  `kiln_surface.c` opened both entry points with
  `assertf(id < KILN_SURFACE_MAX)` where `id` is a `uint8_t` and the max is
  256, so the guard could never fire and had been reported to nobody for as
  long as the module existed. It is now a `_Static_assert` on the invariant
  that actually holds it — shrink the table below 256 and the build stops,
  rather than silently reopening an out-of-bounds index behind a check that
  reads like it covers you.
- **The host build reproduces upstream's bugs on purpose.**
  `plat/host/include/t3d/t3dmath.h` keeps Tiny3D's `T3D_DEG_TO_RAD` **without**
  parentheses around its parameter, exactly as upstream has it, and the host
  text layer will keep libdragon's missing `@` glyph missing. A host header that
  quietly fixes an upstream bug stops predicting what the console does, which
  is the only thing it is for.
- **The host build cannot see fill rate.** That is the console's binding
  constraint and it has no host analogue, so a PC run is never evidence that
  content is affordable. What the host CAN do is assert the limits that are
  silent on hardware — TMEM occupancy, the 70-vertex cache, matrix depth — so
  the direction of travel is a host build that is *stricter* than the console,
  not laxer.

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
- **A model's ORIGIN is a convention, and nothing checks it.** A downstream
  game's rig once exported with the origin between the character's feet while
  every consumer placed it by the hip — inherited from the rigid model the
  skinned one replaced, whose origin was the root bone. One draw call used a
  "standing hip height" constant, another used `0.0f`, and neither was wrong
  on its face: the character's feet floated most of a body-height above the
  floor and its head passed through the ceiling. **The only symptom was
  cameras aimed at it photographing empty room** — which reads as a framing
  problem, and cost a full pass of camera retuning that was treating a
  symptom. The fix belongs in the exporter (rebase the mesh onto the root
  pivot, once), with the origin height published by the generator and a check
  asserting a call site still agrees with it — not an offset re-derived at
  each call site. Note `kiln_transform_push` rotates about the model origin
  with no pivot offset, so an origin at the feet also *fells* a body that
  pitches to lie down instead of laying it flat — the origin is not only an
  offset.
- **An asset builder's `name` is the FILENAME the ROM must open.** It is not a
  label. `mkRawAsset { name = "foo"; dest = "maps"; }` produces
  `maps/foo.map`, and a ROM asking for `rom:/maps/foo_bar.map` gets nothing.
  Every layer below is written to survive a missing asset — `kiln_map_load`
  returns non-zero instead of asserting, an empty clip world makes every
  trace report "nothing in the way" — so the composition is silent instead of
  a build failure. This has cost a downstream game an entire screen once
  already. Grep the C for the path before choosing a `name`.
- **Two generators must not publish the same macro name.** Two independent
  header generators in one downstream game once emitted the same six macro
  names for two DIFFERENT rooms — one procedural and unused, one the real
  shipping geometry — with different numbers. Which room a shared name
  described came down to **include order**, silently, behind a
  `-Wmacro-redefined` warning invisible in a build that ships `-Wno-error`.
  A generated header is a namespace, not just a file — name it like one.
- **A module compiled out under `KILN_DEBUG` breaks every debug ROM's link.**
  `nix/engine.nix` builds `libkiln.a` exactly ONCE, without `KILN_DEBUG`, so a
  `.c` wrapped in `#ifdef KILN_DEBUG` contributes no symbols — and a ROM built
  with `debugConsole = true` then fails to link against the header's real
  declarations. The house pattern (`kiln_console.h` states it, `kiln_debugdraw.h`
  repeats it) is: symbols always present, `--gc-sections` drops them if unused,
  the *game* gates its own call sites.
- **`FONT_BUILTIN_DEBUG_MONO`'s `@` is not missing — it is unreadable.** This
  entry used to say the font had no `@` glyph and drew it as `0`, so `"2@5.4"`
  came out as `"205.4"`. Half of that is wrong and half is worth keeping.
  Decoding the pinned libdragon's own font blob: **all 95 printable ASCII
  codepoints 0x20–0x7E have glyphs**, `@` among them, and `0`, `O` and `@` are
  three genuinely different bitmaps — a slashed zero, a plain oval, a ringed
  at. The 34 codepoints with no glyph are all above 0x7E.
  But rendered at 6 px advance in a 7x9 box, `@` reads as a slightly decorated
  `0`, which is exactly how `"2@5.4"` got transcribed as `"205.4"` in the first
  place. So the original advice stands and the original explanation does not:
  keep anything meant to be **read off a capture and retyped** to alphanumerics
  plus `.` and `-`, because the failure is the eye, not the font.
  The font is monospaced at a 6 px advance with no kerning, ascent 11, descent
  −2 — which is also what makes host text extents exact arithmetic.
  `nix/checks/kiln-font.nix` pins all of it, `@` != `0` included.
  Worth noting how this was settled: by rendering the string through the host
  2D pass and magnifying the capture. That is the first thing the host build
  has told us about the console that no ROM boot had.
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
  Adding it cost the KS voice 291 → 333 weighted cycles (whole object); the
  frame-scoped count is 259. That is the price of not trapping into the
  denormal exception handler.
- **Faust `-os` emits `frame()` and leaves `compute()` an EMPTY STUB.** An
  architecture file written against `compute()` builds, links, runs, and outputs
  silence.
- **ROM titles need literal quotes** in the make variable (`N64_ROM_TITLE =
  "Kiln Hello"`) because `n64.mk` interpolates `n64tool -t $(N64_ROM_TITLE)`
  unquoted. Without them a title with a space fails with "Need output flag
  before first file".
- Cross artifacts need `dontStrip` / `dontPatchELF`; nixpkgs' fixup phase
  otherwise strips the debug ELF that `n64sym` needs for backtraces.
- **`gltf_to_t3d` aborts on a glTF material with no fast64 data** ("Material
  has no fast64 data! (@TODO: implement fallback)", `terminate called after
  throwing... std::runtime_error`). It expects the custom properties
  Blender's fast64 add-on writes on export. Two ways out, both used in this
  repo: `mkModel`'s `ignoreMaterials = true` (passes `--ignore-materials`,
  what `assets/cube.gltf`/`nix/assets.nix`'s `demoModel` use — untextured,
  no material data needed at all), or `tools/f3d_inject.py`, which writes a
  real `f3d_mat` block directly without the actual addon (see "Geometry
  authoring" above) — what everything under `nix/blender.nix` uses.

## The gates (nix/faust.nix, nix/checks/)

Verified to fire in both directions — a clean voice passes, a voice using
`ma.tanh` fails with an actionable message.

The no-libm gate inspects **undefined symbols in the compiled object**, not
source text. This is deliberate and better: `floorf` gets inlined to one
instruction (a source grep would report a cost that isn't there), while Faust's
internal `fmaxf`/`fminf` never appear in the `.dsp` at all. `-ffast-math`
(which libdragon enables) is what inlines them.

The cycle-budget number is a **static estimate** — instruction counts weighted
by the report's §2 VR4300 latency table, scoped to the `frame<name>` function
only. It cannot model cache or the ~640 ns RDRAM latency. It exists to catch
regressions, not to predict wall-clock. Say so whenever quoting it; profile
with `TICKS` on hardware for real numbers. The gate is a **hard failure**
when the frame-scoped weighted cycles exceed the declared budget.

### The full check list (88 checks, 24 implementations)

`rom.nix` ×24 (magic / title / size), plus `toolchain`, `streamdb`,
`kiln-asset`, `assets` (determinism), `mapmaker-roundtrip`, and five that are
worth knowing by name:

- **`kiln-names`** fails the build if `m64`/`M64` comes back as a name for this
  engine. A rename is an invariant, not a state; without a gate the tree drifts
  back one comment at a time. It strikes each allowlisted phrase out of a line
  before searching it, which is what lets `xm64player_stop(&g_music)` pass while
  a line carrying both that and a stray `kiln_`-should-be still fails.
- **`kiln-parity`** holds `HOST_MODULES` exact, above. Self-verifying: there is
  no way to make it pass by weakening it. It earned itself the day the 2D tier
  landed, failing with *"kiln_gui compiles natively but is NOT in
  HOST_MODULES"* — a module that had just become host-clean and would otherwise
  have gone a year without `-Werror`.
- **`kiln-map`** loads `assets/quake_test.map` through the real `kiln_map.c`
  off the host VFS, installs it into the real clip world and renders it — the
  first time a level here has been drawn outside a ROM. It proves the NEGATIVE
  first, because that is the failure this project actually had: a missing map
  must be a loud miss, not a silent empty world. It found two defects in
  `kiln_map` and pins both; see the hard-won facts.
- **`kiln-model`** converts `assets/cube.gltf` with the SAME `gltf_to_t3d` the
  ROM build uses, then parses and renders the result on the host. The `.t3dm`
  is deliberately not committed — it would go stale the first time the
  converter changed. The structural assertions matter more than the pixels
  here: a misread offset in a format reader does not fail, it produces
  geometry.
- **`kiln-voxmesh`** renders a real voxel mesh with two different combiners and
  keeps both captures, which is how "Forge's atlas is never sampled" stopped
  being an inference and became a picture. See the Forge section above.
- **`kiln-splash`** runs the real `kiln_splash_init/update/apply/draw3d/draw2d`
  sequence — the engine's actual boot splash, not a stand-in — against a
  `.t3dm` converted from `tools/blender/kiln_logo.py`'s own generated glTF,
  and holds the settled frame (kiln, flame, and the flame-lit publisher line
  all on screen at once) to a reference. Asserts the three named objects
  (`"kiln"`, `"flame"`, `"plate"`) are found *before* diffing pixels: a
  lookup miss makes `kiln_splash_draw3d` fall back to drawing the model as
  one rigid piece, which is correct for a foreign model and silently wrong
  for this one, and a bare pixel diff could only ever report "the frame
  changed" for that failure, not why.
- **`kiln-scene`** is the whole-frame gate: `kiln_frame_begin` →
  `kiln_scene_begin` → geometry → `kiln_gui_begin` → HUD → `kiln_frame_end`,
  run by the real `kiln_engine.c`. Before it, nothing outside a ROM on hardware
  had ever executed that bracket. It caught its own reason for existing on the
  first run: the cubes rendered inside-out, which looked exactly like a broken
  depth compare and was in fact a missing `rdpq_mode_zbuf(true, true)` in the
  shim — **nothing in `kiln_engine.c` calls it, because Tiny3D's own
  `t3d_frame_start` does** (`t3d.c:176`). `T3D_FLAG_DEPTH` is the RSP's half of
  depth; `rdpq_mode_zbuf` is the RDP's, and only one of them is visible in
  engine code. Same two-halves shape `kiln_scene_begin`'s comment describes for
  fog.
- **`kiln-gui`** is the 2D frame gate: the real `kiln_gui.c`, rendered through the
  host rasteriser, diffed against a committed capture AND a text manifest. Two
  files because they fail differently — the PNG catches geometry, and the
  manifest catches the thing a pixel diff reports worst. A one-pixel baseline
  shift lights up every glyph in an image diff and says only "text changed";
  the manifest says *which label moved and where*. Verified firing on both.
- **`kiln-widget`** renders `kiln_widget`'s five screens through the same host
  backend (`tools/uipreview`) and diffs them. It exists because that harness
  used to implement `kiln_gui`'s primitives ITSELF, over a private
  `<libdragon.h>` shim and a hand-rolled 3x5 font — so the tool whose whole job
  was judging layout disagreed with the console about panel edge order, bar
  inset, and whether `kiln_gui_rect` blends alpha at all. **It does not:** with
  the blender off, which is where `kiln_gui_begin` leaves it, the RDP ignores
  source alpha and writes opaque, so `kiln_widget`'s translucent background
  "motes" are solid squares on hardware. A preview that draws its own pixels is
  a second implementation of the thing it is previewing.
- **`kiln-font`** regenerates `plat/host/include/kiln_host_font.h` from
  libdragon's own font blob and diffs, then asserts what a diff cannot: still
  monospaced at 6 px, every printable ASCII codepoint present, every glyph
  carrying both fill and outline pixels — that last one is what proves the
  2bpp layer decode picked the right half, since the wrong half yields glyphs
  built out of their atlas slot-mate's pixels.
- **`kiln-*-wasm32`** are the same six render gates (`gui`, `scene`, `model`,
  `map`, `splash`, `voxmesh`) plus `wav64`, compiled by `emcc` and run under
  node, held to the **same** committed reference files as the native ones. A
  per-architecture reference would only prove each architecture agrees with
  itself. `./dev arch aarch64` / `riscv64` runs three of the bodies
  cross-compiled against musl under qemu-user and is out of the gate set
  because a cross toolchain is a ~170 MB fetch.
- **`kiln-shell`** runs the real `examples/engine/main.c` game loop under the
  launcher for ninety frames on SDL's dummy drivers. Ninety and not one: one
  frame proves the linker found everything, ninety proves the loop comes back
  round, which is exactly what `audio_can_write`'s `return 1` prevented. It
  checks statistics rather than a golden image because that ROM's HUD prints a
  smoothed frame rate off a real clock, so two runs cannot match.
- **`kiln-web`** runs the BROWSER launcher — the canvas blit and the ASYNCIFY
  game loop — under node against a recording DOM stub, and asserts twelve
  frames reached `putImageData` at 320x240 with content in them. It also
  withholds `AudioContext`, because that is the state every browser tab is in
  before the user's first gesture and a launcher that hard-required audio
  would hang before the first frame.
- **`kiln-wav64`** decodes a real `audioconv64` asset and measures **RMS**, not
  sample count: a decoder can return the right number of samples full of
  zeroes and pass every structural assertion, and silence is the least
  attributable failure in the audio path. It also asserts pan 0.0 is hard
  left, which backwards is audible, deniable and never reported.
- **`kiln-hostmath`** asserts that the host build of libdragon's `fm_*` computes
  what the VR4300 computes. `nix/host-math.nix` substitutes four libm calls for
  four MIPS instructions; this sweeps 4,001 inputs against libm and pins the
  tie-breaking rule (`round.w.s` is ties-to-**even**, so `nearbyintf` and not
  `roundf`). Verified to fire on exactly that distinction.
- **`blender-tests`** runs every `tools/blender/test_*.py` — the bpy-free
  geometry builders, in milliseconds. This is the fastest feedback loop in the
  project and was ungated for most of its life; one test in the glob had been
  failing on a missing `bpy` unnoticed the whole time. `blender_test_*.py` is
  the *other* naming, for tests that genuinely need a live Blender and are
  deliberately excluded.
- **`forge-roundtrip`** runs `tools/forge/frg.py`'s selftest — the host mirror of
  `kiln_voxel_boxes`, asserted to partition the solid set exactly once — then
  imports a committed `.map`, re-emits it twice for byte-equal idempotency, and
  passes the result through `quake_map.py`'s **strict** CSG. That last step is
  the one that matters: the console parser componentwise min/max's plane points
  and so loads any winding, while the CSG needs correct outward normals. Its
  first run caught a texture-name regex that matched `(` instead of the texture,
  so every brush re-imported as block type 1 — a level round-tripping back as one
  material.
- **`kiln-logic`** compiles `kiln_clip`, `kiln_dict`, `kiln_cache`, `kiln_lod`,
  `kiln_rng` and `kiln_voxel` **natively at `-Werror`** against `nix/checks/stub/` and asserts on
  them. Put a new module's pure logic here. On its first run it found two real
  bugs `nix build` had been passing for months: `kiln_cache`'s handle 0 being
  both "slot 0" and "invalid" (so the first asset acquired looked like a load
  failure) and `kiln_clip`'s broadphase populating grid cells with the wrong
  brushes (so a player walked through two of four walls). Compiling natively at
  `-Werror` is also a free second opinion on the engine's own `-Wno-error`.
**A gate should be verified to fire in both directions.** The no-libm gate and
the cycle budget both have been (a clean voice passes, a voice using `ma.tanh`
or emitting a double-precision instruction fails with an actionable message);
a check that has only ever been seen to pass is a check that might not be
checking anything. `kiln_camlint` and `kiln_camkey` (the generated-header/
generator-agreement pattern, and the camera-curve invariant checks) were
proved out this way against a downstream game's content before either module
moved into the engine proper — the pattern is reusable by any game shipping
generated headers or authored camera curves, via the same `nix/checks/`
convention `kiln-map`/`kiln-voxmesh`/`kiln-logic` already show below.

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
- **The engine runtime has no model/asset loading yet** (no `kiln_object`
  layer). The asset *pipeline* is built (`nix/assets.nix`, verified by
  `examples/assets-demo`), and `kiln_asset` provides StreamDB loading for
  models and sprites — but nothing in `libkiln` auto-loads assets. A ROM
  that wants a model loads it by hand with `t3d_model_load` or
  `kiln_asset_model`, as `examples/assets-demo/main.c` does.
- **No collision system of any kind.** *(Phase D built one — `kiln_clip.h`'s
  slab-method swept-AABB + SlideMove, plus `kiln_player`'s locomotion state
  machine and `kiln_camera`'s collision-aware boom that raycasts against it.
  This entry kept for the historical record of what Phase B left out; see
  Phase D above for what's there now.)* What's still missing: no
  rotation/contents/contact-point traces (Doom 3's `Rotation`/`Contents`/
  `Contacts`), no capsule with hemispherical caps (only AABB), and
  `kiln_room`'s `user_mesh` is still drawn but never auto-queried — a room
  has to install its brushes into the clip world itself (as
  `examples/oot-demo` does at boot). A future `kiln_room` integration would
  concatenate loaded rooms' brush arrays into one module-static buffer on
  load/unload, so `kiln_clip_set_world` call sites stop being per-ROM.
- **`kiln_asset_wav64` is not provided** — libdragon's `wav64_open` is
  path-only with no in-memory variant, so audio assets must use DFS
  (`rom:/` paths), not StreamDB. Lands when `wav64_open_buf` lands upstream.
- **XM64 / YM64 tracker playback is bookkeeping on the host.** `kiln_music_*`
  reserves and reports channels correctly and produces no notes: libdragon's
  XM player is not separable from the RSP mixer the way the VADPCM codec is.
  SFX are real (see "One renderer, four architectures").
- **EMULATOR screenshot verification is not part of `nix flake check`** — it
  needs a live Wayland session, which the build sandbox does not have, so
  `./dev shot` stays a desktop command. **Host-render verification now is**:
  `nix/checks/kiln-gui.nix` draws a HUD with the real `kiln_gui.c` through the
  software rasteriser in `plat/host/src/host_gfx.c` and diffs it against a
  committed PNG and text manifest (`nix/checks/refs/`). No session, no driver,
  no compositor, byte-identical across runs. The console is still the arbiter
  of what a frame really looks like; the gate holds the 2D pass's own
  arithmetic still.
- **Hardware deploy is unverified against a physical cart** — `sc64deployer`
  builds and runs (`sc64deployer list` → "No SC64 devices found"), but no cart
  was attached during development, so `./dev deploy`, `./dev debug`, and the
  udev module are untested on real hardware.
- Normalisation is not offered for baked instruments; you set `gain` explicitly
  and the clipping gate tells you when it is wrong.

## Conventions

Every file carries an `SPDX-License-Identifier`. The engine is **MIT**,
engine-wide — see `LICENSE`. The one exception is `streamdb-embedded/`
(LGPL-2.1-or-later, DeMoD's own library, source vendored unmodified under its
own headers, unaffected by the relicense). libdragon (Unlicense) and
SummerCart64 (GPLv3) are pulled in only as Nix flake inputs, never vendored,
and packaged as separate programs — neither is linked into a ROM. Faust
`.dsp` sources from `~/Documents/DeMoD/apps/terminus/patches/` are
**PolyForm Shield 1.0.0, non-commercial** — reference them, do not vendor
them into this tree. See `THIRD_PARTY_LICENSES.md` for the full breakdown.

M64 (ModRetro's console) facts are as of the July 2026 launch window and
ModRetro ships OTA updates, so anything stated here about ITS behaviour is a
snapshot, not a guarantee.
