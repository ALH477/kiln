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
nix/engine.nix      libm64 — the M64 engine (3D on Tiny3D, 2D GUI on rdpq,
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

## Phase B — actors, rooms, camera, skeletal animation

The "runtime" half of the engine, on top of the 3D/GUI layer above: live
game objects (`m64_actor.h`), streamed world geometry (`m64_room.h`), an
OoT-style follow camera (`m64_camera.h`), and skeletal animation
(`m64_skel.h`). Verified by `examples/actors-demo`, `examples/rooms-demo`,
and `examples/camera-skel-demo` (camera + skel + `m64_audio` together).
Audio itself is documented separately below; the connective tissue between
Phase B and audio is `m64_room_current()` feeding
`m64_audio_set_room_music`/`update_rooms` (see "The audio layer").

Each module is modelled on a specific piece of Ocarina of Time's engine, and
each header's own comment says so and explains what was deliberately left
out. The summary, so it's in one place:

- **`m64_actor.h`** — OoT's Actor/ActorProfile split: a flat, caller-owned
  pool (not malloc-per-actor), category-ordered update/draw lists (player
  before enemies before props, same reasoning OoT gets predictable draw
  order and cheap category queries from), one fixed-size inline state block
  per instance sized to the largest actor type (`M64_ACTOR_STATE_MAX`,
  override before including the header if 64 bytes is too small — this is
  OoT's "instance struct sized to the overlay's max" idea), and
  handle+generation instead of raw pointers so a stale reference resolves to
  NULL instead of a reused slot. **Not carried over:** OoT's overlay/segment
  paging — actor code is never paged in and out of a small ROM window here,
  every actor type's code is resident for the whole game, which is the
  simpler and correct choice until a game's actor code genuinely doesn't fit
  in RAM at once. No collision/physics system of any kind yet (see "Not yet
  built").
- **`m64_room.h`** — OoT's cross-shaped loaded-room set: AABB-overlap with
  the camera (not a radius, which would also pull in both diagonal
  neighbours at every corner) plus each room's `neighbours[]` table as
  streaming candidates, actor spawn templates deferred to room-load time so
  an actor is never resident in RAM without a mesh to occlude it against.
  **Not carried over:** no BG collision mesh streamed per room — there is no
  collision system at all yet, so `user_mesh` is drawn but never collided
  against.
- **`m64_camera.h`** — OoT's follow-camera boom: a fixed-length spring arm
  behind the target whose *heading* lags the target's facing direction on
  its own damper, separate from the eye position's damper, so a sharp turn
  swings the camera around over several frames instead of snapping it (see
  `Camera_Normal1` in the OoT decomp for the shape of this). Phase 5 grew
  the two things Phase B deliberately left out: a **mode stack**
  (`m64_camera_push`/`pop`, `M64_CAM_NORMAL`/`TARGETING`/`CUTSCENE`,
  fixed 4-deep, restores mode + smoothed state together) and a
  **collision-aware boom** (`m64_camera_set_collision` opt-in; the boom is
  `m64_clip_ray`'d against the world each frame and pulled in on a hit).
  Both are opt-in and default-OFF so Phase B examples link and behave
  unchanged. Also a deliberate engine-wide departure: damping is
  linear-per-frame (`t = min(1, speed*dt)`), not `expf(-t)` — same
  qualitative curve for one multiply instead of a transcendental call,
  consistent with this engine's "single precision, no gratuitous libm"
  stance (see "Constraints that shape everything").
- **`m64_skel.h`** — not modelled on OoT (which predates glTF-style skinning
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

## Phase C — runtime asset streaming (engine/src/m64/m64_asset.*, examples/streamdb-demo)

`m64_asset.h` is the runtime half of a gap CLAUDE.md itself used to describe:
`nix/assets.nix`'s build-side pipeline (`mkModel`/`mkSprite`/...) already
produced converted assets, but nothing in `libm64` opened a StreamDB
container at runtime — every ROM that wanted an asset loaded it by hand
with `t3d_model_load`/`sprite_load` against a DFS path. `m64_asset` closes
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
  tables, actor params, and — via `m64_asset_model`/`m64_asset_sprite` —
  any model or sprite a game wants to find by suffix scan ("every `.t3dm`
  in this DB") instead of a hardcoded path per asset.

**One arena, caller-owned, no cache.** `m64_asset_open` takes a caller-sized
arena (`m64_asset_probe_size` sizes it); a heap failure mid-level is not
recoverable on this console, so sizing happens at boot, not lazily.
`m64_asset_model`/`m64_asset_sprite` malloc and return — the caller holds
the pointer, same contract as `t3d_model_load`. No cache table; a bounded
one is a same-day follow-up if an actor type ever needs on-demand loading,
not needed by anything built so far.

**`m64_asset_model` needs `t3d_model_load_buf`,** which Tiny3D upstream does
not expose (`t3d_model_load(path)` only) — `nix/patches/tiny3d-load-buf.patch`
adds it as a pure refactor (extracts `t3d_model_load`'s body into
`t3d_model_load_buf(buf, sz)`) so a `.t3dm` already read out of a StreamDB
payload can be parsed without a round-trip through DFS. `nix flake check`
fails loudly here, not silently at runtime, if a Tiny3D bump ever makes the
patch stop applying.

**`m64_asset_wav64` is deliberately NOT provided** — see "Not yet built".

Verified by `examples/streamdb-demo`: one `.streamdb` packing a model, a
sprite, and a raw level-layout blob, exercising `m64_asset_model` (the
patched load-from-buffer path), `m64_asset_sprite`, `m64_asset_load` on the
raw blob, and `m64_asset_find_suffix`.

## Phase D — OoT + id Tech 4 feel (input, clip, dict, map, surface, sound, event, target, player)

The "feel" half of the engine, layered on Phase B: a clean-room
implementation of the primitives that make a game feel like Ocarina of Time
to play *and* like id Tech 4 (Doom 3) to author for. Eight new modules, two
existing modules extended additively. Each module's own header comment
names the id Tech 4 / OoT primitive it models and what was deliberately
left out. Verified by `examples/clip-demo`, `examples/map-demo`,
`examples/event-demo`, and the Phase 6 integration proof
`examples/oot-demo`.

- **`m64_input.h`** — one-poll-per-frame joypad wrapper. Squared-magnitude
  deadzone (a disc, not a square — a per-axis threshold makes the stick
  report motion on a resting diagonal). Button edges (`edges`/`released`)
  computed by diffing against last frame, so example code stops
  hand-rolling XOR. Replaces direct `joypad_poll` everywhere new.
- **`m64_clip.h`** — idPhysics / CollisionModel trace analogue. World = a
  flat array of brush AABBs per loaded room (no BSP — overkill for OoT-room
  counts on a 4 MB console). Slab-method swept AABB vs AABB, single
  precision with a 1e-3 epsilon (s16.16 world scale; 1e-4 produces visible
  contact jitter). `m64_clip_slide` is the iterative clip-and-retry
  SlideMove shape (Doom 3's `idPhysics_Player::SlideMove`) — what makes a
  player slide along a wall instead of stopping dead. No rotation traces,
  no contents test, no contact-point list — those are layered on top by a
  game that needs them.
- **`m64_dict.h`** — idDict analogue. Module-global interned key table
  (256 caps, one boot allocation); per-instance `M64Dict` is a fixed
  16-slot array of `{key_id, type, union{int,float,fm_vec3_t,str_id}}`.
  Embedded in `M64RoomSpawn` so spawn args ride with the spawn template.
  `m64_dict_set_auto` parses "0 0 0" as vec3, "5.5" as float, "5" as int,
  else string — the auto-typing idDict's `Set` does on text input.
- **`m64_map.h`** — idMapFile analogue. Parses the existing Quake `.map`
  text format (`assets/quake_test.map`, `assets/oot_test.map`). One-pass
  tokenizer: entity `{ "k" "v" ... <brushes> }`; brush blocks reduced to
  AABB (componentwise min/max of plane points) + one parallelogram per
  face. Non-axis-aligned faces render as parallelograms, not true polygons
  — flagged as a known limit, fine for rectangular OoT-style rooms.
  `classname` → `profile_id` via `m64_map_register_classname`. One `.map`
  = one room for the demo; multi-room games load several `.map` files and
  connect them via `target_room` epairs later.
- **`m64_surface.h`** — surface-prop table analogue. `M64SurfaceDef[256]`
  of `{ friction, footstep_sfx, render_flags }`, indexed by
  `M64Trace.hitsurface`. A real Doom 3 binds materials to textures with
  surface flags (metal, flesh, stone); on an N64 with no programmable
  pixel pipeline the "material" layer is one small fixed table the gameplay
  code reads, separate from the rdpq combiner the renderer uses.
- **`m64_sound.h`** — sound-shader analogue, separate from `m64_audio.h`
  for clarity. `M64SoundShader { name, wav64_path, base_vol,
  falloff_radius, loop }`; `m64_sound_play(name, world_pos, pitch)`
  computes distance→volume and listener-facing→pan (stereo only — no HRTF
  on a 93.75 MHz VR4300) and triggers `m64_sfx_play_ex`. One
  `m64_sound_update_listener` per frame; looping positional shaders
  (torches, machines) recompute vol/pan from it.
- **`m64_event.h`** — idEvent analogue. One flat pool of 256 slots (~7 KB)
  and a single `m64_event_process` per frame — per-actor queues would mean
  per-actor malloc, which the engine deliberately never does (see
  `m64_actor.h`'s flat-pool rationale). `m64_event_post(handle, event_id,
  delay_ms, args, argc)`; `m64_event_process(dt)` runs BEFORE
  `m64_actor_update_all` so events land before the actor's own update.
  Pool-full policy: a new event with priority higher than the
  lowest-priority queued event evicts that one (debugf'd); otherwise the
  new event is dropped (debugf'd). Stale targets (despawned before fire)
  are dropped silently — a queued "play idle" event for a killed actor is
  not a warning worth spoiling real bugs with. Dispatched via
  `M64ActorEventFn` on `M64ActorProfile` (NULL = ignore).
- **`m64_target.h`** — Z-targeting. Cone + range query over the ENEMY and
  NPC category lists (reuses `m64_actor_first/next` — no spatial index, no
  kd-tree; at OoT enemy counts per room the linear walk is cheaper than
  maintaining a structure). `m64_target_acquire` picks the smallest-angle
  candidate in the forward cone; `m64_target_switch` cycles by stick
  direction; `m64_target_draw_reticle` projects the locked actor's world
  position through the scene's view basis and draws four corner brackets
  via `m64_gui`, clamping to the nearer screen edge when the target is
  behind the camera.
- **`m64_player.h`** — the player locomotion state machine. A helper, not
  an actor profile: the player IS an actor (category PLAYER), and its
  profile update/draw call into `m64_player_*` which owns the
  IDLE/WALK/RUN/ROLL/ATTACK/JUMP/FALL machine. Reads `m64_input_get(port)`,
  integrates velocity against `m64_clip_slide`, probes ground with
  `m64_clip_ground`, and posts `M64_EV_PLAYER_FOOTSTEP` events at a
  cadence proportional to speed — the actor's `M64ActorEventFn` dispatches
  to `m64_sound_play` keyed by the underfoot surface. Camera-relative
  movement basis set each frame via `m64_player_set_camera_basis`.

**Existing modules extended (additively, backward-compatible):**
- **`m64_actor.h`** — `M64ActorProfile` gained `M64ActorEventFn event` and
  `m64_actor_dispatch_event` (used by `m64_event_process`). `m64_actor_spawn`
  takes a `const M64Dict *dict` (may be NULL) the profile's `init` reads
  spawn args from. Old examples pass NULL and behave as before.
- **`m64_camera.h`** — mode stack + collision-aware boom (see Phase B
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
(sliding via `m64_clip`), Z-targets two orbiting enemies (camera pushes
`M64_CAM_TARGETING`, reticle projects through the scene), emits footstep
SFX via `m64_event` + `m64_sound`, and gets a 1.5 s `M64_CAM_CUTSCENE`
pan on boot that pops back to NORMAL — every Phase D module in one frame.
`nix build .#oot-demo` and `nix flake check` are green (32 checks).

## The audio layer (engine/src/m64/m64_audio.*, examples/audio, examples/live-voice, examples/music)

Three audio paths, all first-class:

```
Baked instruments (report Stage 1, recommended 80-90%):
  .dsp → mkBakedInstrument → host render (-double) → audioconv64 → .wav64
       → mkN64Rom `assets` → DragonFS → m64_sfx_load/play → RSP mixer

Live Faust voices (report Stage 2):
  .dsp → mkFaustVoice → faust -lang c -single -os → VR4300 object
       → linked into ROM → faust_n64_<name>_render(out, n, accumulate)
       → VR4300 summing into AI buffer alongside mixer_poll

Tracker music:
  .xm/.ym → mkMusic → audioconv64 → .xm64/.ym64
         → mkN64Rom `assets` → DragonFS → m64_music_load/play → RSP mixer
```

The engine audio layer (`m64_audio.h`) wraps libdragon's RSP mixer with:
- `m64_audio_init/update/close` — init, per-frame pump, teardown
- `m64_sfx_load/play/play_ex/stop` — SFX with priority-based voice stealing
- `m64_music_load/play/stop/set_volume` — XM64/YM64 tracker music
- `m64_audio_set_room_music/update_rooms` — room-based music crossfading

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

## Geometry authoring (nix/blender.nix, tools/blender/, tools/blender-mcp/)

**Blender authors geometry; it does not author materials.** `nix/blender.nix`
(`mkBlenderModel`) drives Blender headless (`--background`) with a script
from `tools/blender/` that builds a scene via `m64lib.py`'s helpers
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
Tiny3D hardware constraint `m64_skel.h`'s Phase B notes describe (§ above).

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
  Adding it cost the KS voice 291 → 333 weighted cycles (whole object); the
  frame-scoped count is 259. That is the price of not trapping into the
  denormal exception handler.
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
- **The engine runtime has no model/asset loading yet** (no `m64_object`
  layer). The asset *pipeline* is built (`nix/assets.nix`, verified by
  `examples/assets-demo`), and `m64_asset` provides StreamDB loading for
  models and sprites — but nothing in `libm64` auto-loads assets. A ROM
  that wants a model loads it by hand with `t3d_model_load` or
  `m64_asset_model`, as `examples/assets-demo/main.c` does.
- **No collision system of any kind.** *(Phase D built one — `m64_clip.h`'s
  slab-method swept-AABB + SlideMove, plus `m64_player`'s locomotion state
  machine and `m64_camera`'s collision-aware boom that raycasts against it.
  This entry kept for the historical record of what Phase B left out; see
  Phase D above for what's there now.)* What's still missing: no
  rotation/contents/contact-point traces (Doom 3's `Rotation`/`Contents`/
  `Contacts`), no capsule with hemispherical caps (only AABB), and
  `m64_room`'s `user_mesh` is still drawn but never auto-queried — a room
  has to install its brushes into the clip world itself (as
  `examples/oot-demo` does at boot). A future `m64_room` integration would
  concatenate loaded rooms' brush arrays into one module-static buffer on
  load/unload, so `m64_clip_set_world` call sites stop being per-ROM.
- **`m64_asset_wav64` is not provided** — libdragon's `wav64_open` is
  path-only with no in-memory variant, so audio assets must use DFS
  (`rom:/` paths), not StreamDB. Lands when `wav64_open_buf` lands upstream.
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
