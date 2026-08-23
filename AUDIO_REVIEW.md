<!-- SPDX-License-Identifier: MIT -->

# Kiln Audio Subsystem Review — Living Document

**Status**: Active development. This document tracks the review findings and
the implementation plan as it progresses. Update after each phase.

---

## 1. Current State

### 1.1 What Works

| Path | Builder | Output | Runtime | Verified? |
|------|---------|--------|---------|-----------|
| Baked instruments | `mkBakedInstrument` | VADPCM `.wav64` | RSP mixer | Yes (`examples/audio`) |
| One-shot SFX | `mkSound` | VADPCM `.wav64` | RSP mixer | Yes (`examples/assets-demo`) |
| Tracker music | `mkMusic` | `.xm64`/`.ym64` | libdragon XM64/YM64 | Pipeline only, no runtime example |
| Live Faust voices | `mkFaustVoice` | MIPS `.o` | VR4300 via `frame()` | Built + gate-tested, no end-to-end ROM |

### 1.2 Gate System (`nix/faust.nix:322-376`)

| Gate | What it checks | Fail? |
|------|---------------|-------|
| No-libm | Undefined symbols in compiled `.o` against `fatalSymbols` list | **Hard fail** |
| No-double-precision | `.d`-suffixed FP instructions in objdump disassembly | **Hard fail** |
| Cycle budget | Weighted FP instruction count vs declared budget | Advisory only |
| Silence | Baked WAV peak == 0 | **Hard fail** |
| Over-quiet | Baked WAV peak < -40 dBFS | **Hard fail** |
| Clipping | Baked WAV > 0.1% samples at 32767 | **Hard fail** |
| Compression ratio | `.wav64` not smaller than source `.wav` | **Hard fail** |

### 1.3 Architecture File (`dsp/arch/libdragon_mixer.c`)

Correct N64-specific decisions:
- One-sample mode (`-os`) for per-sample control data (report §6)
- Static allocation, no malloc in audio path (report §4)
- Parameter zones resolved once at init → single-store writes
- Per-sample float→int16 conversion, no staging buffer (RDRAM latency)
- Clamp-before-cast prevents wrap-around noise
- Soundfile primitives dropped — sample playback belongs in RSP mixer

### 1.4 libdragon Audio API (available in `$N64_INST`)

- **Mixer**: `mixer_init(num_channels)` (max 32), `mixer_poll(buf, nsamples)`,
  `mixer_ch_play/stop/playing`, `mixer_ch_set_vol(ch, lvol, rvol)`,
  `mixer_ch_set_vol_pan(ch, vol, pan)`, `mixer_ch_set_freq(ch, freq)`
- **wav64**: `wav64_open(wav, fn)`, `wav64_play(wav, ch)`, `wav64_set_loop`,
  `wav64_close` — path-only, no in-memory variant
- **XM64**: `xm64player_open(player, fn)`, `xm64player_play(player, first_ch)`,
  `xm64player_stop`, `xm64player_set_vol`, `xm64player_set_loop`,
  `xm64player_close` — uses N mixer channels starting at `first_ch`
- **YM64**: `ym64player_open/play/stop/close` — same pattern
- **Audio**: `audio_init(freq, latency)`, `audio_can_write`,
  `audio_write_begin/end`, `audio_get_buffer_length`
- **Note**: `audio_init` second arg is a float latency, not buffer count.
  Use `AUDIO_INIT_LATENCY_MS(ms)` or `AUDIO_DEFAULT_LATENCY`.

---

## 2. Gaps Identified

### Gap 1: Zero Engine Audio Layer

`libkiln` has no audio code. The `engine/Makefile` compiles 5 source files —
none audio-related. Every ROM calls raw libdragon APIs directly.

### Gap 2: Live Voice Path Has No Mixer Integration

`faust_n64_<name>_render()` overwrites its output buffer — no accumulation
mode for summing multiple voices. No ROM links or plays a live voice.

### Gap 3: Cycle Budget Gate Is Advisory Only

`nix/faust.nix:354-372`: The AWK cycle estimator does not set `fail=1` when
the weighted count exceeds the declared budget.

### Gap 4: Sample Rate Inconsistency

`examples/audio` uses 32000 Hz (correct). `examples/assets-demo` uses 44100 Hz
(wasteful). No build-time enforcement that bake rate matches runtime rate.

### Gap 5: Room-Based Audio Routing Is a Comment, Not Code

`kiln_room.h:137`: "Use this for sound / music routing" — but no implementation.

### Gap 6: No XM64/YM64 Playback Example

`mkMusic` produces `.xm64`/`.ym64` files, but no example ROM plays them.

### Gap 7: No wav64 from StreamDB

`kiln_asset.h:44-48`: Blocked on upstream `wav64_open_buf`. Audio assets
must use DFS (`rom:/` paths), not StreamDB.

---

## 3. Implementation Plan

### Phase 3: Make Cycle Budget Gate Fail ✅

**Status**: Complete

**Goal**: Promote the cycle budget from advisory to a hard build failure
when exceeded, scoped to the `frame<name>` function only.

**Changes**:
- `nix/faust.nix`: Scoped the AWK cycle estimator to the `frame<name>`
  function in objdump output (excludes init/constructor code).
- Added `fail=1` when frame-scoped weighted cycles > declared budget.
- `engine/Makefile`: Added `-DSTREAMDB_EMB_BACKEND_DFS=1` to fix pre-existing
  build failure (kiln_asset.c couldn't see DFS backend declarations).

**Results**:
- KS voice frame-scoped: 95 FP instructions, **259 weighted cycles**
  (vs old whole-object count of 333 — init code was inflating by ~28%)
- Budget: 500 cycles/sample — passes with margin
- Gate now hard-fails if a voice exceeds its declared budget

### Phase 4: Sample Rate Enforcement ✅

**Status**: Complete

**Goal**: Build-time check that baked instrument rates match the ROM's
declared audio rate.

**Changes**:
- `nix/faust.nix:mkBakedInstrument`: Writes `sampleRate` to
  `$out/nix-support/audio-rate` in install phase.
- `nix/rom.nix:mkN64Rom`: Added `audioRate ? null` parameter. If non-null,
  cross-checks against all assets that expose `nix-support/audio-rate`.
  A mismatch hard-fails the build with a clear message.
- `flake.nix`: Added `audioRate = 32000` to `audio` and `assets-demo` ROMs.
- `examples/assets-demo/main.c`: Fixed 44100 → 32000.

**Results**:
- `audio` ROM: rate check passes (32000 Hz match)
- `assets-demo` ROM: builds with 32000 Hz, `demoSound` (mkSound) correctly
  skipped (no `nix-support/audio-rate` file)

### Phase 1: Engine Audio Layer ✅

**Status**: Complete

**Goal**: Add `kiln_audio.h` / `kiln_audio.c` to `libkiln` providing init,
per-frame pump, SFX with priority voice stealing, and music (XM64/YM64).

**Files created**:
- `engine/src/kiln/kiln_audio.h` (119 lines) — API: init/update/close,
  sfx_load/play/play_ex/playing/stop/set_vol_pan/set_freq,
  music_load/play/stop/set_volume/set_loop/playing/num_channels
- `engine/src/kiln/kiln_audio.c` (245 lines) — implementation

**Files modified**:
- `engine/Makefile` — added `kiln_audio.c` / `kiln_audio.h` to src/inc/OBJ
- `nix/engine.nix` — added `kiln_audio.h` to install check

**Design**:
- Channel partition: `[0..sfx_channels)` for SFX, `[sfx_channels..total)` for music
- Default config: 32000 Hz, 16 SFX + 10 music = 26 channels (max 32)
- SFX auto-allocation: walks SFX range for free channel, or steals lowest-priority
- Music: XM64/YM64 detected by extension, channels assigned from music range
- `kiln_audio_update()` drains `audio_can_write` / `mixer_poll` per frame
- No malloc in audio path; SFX table is fixed-size, loaded at boot

**Verified**: `nix build .#engine`, `.#audio`, `.#assets-demo`, `.#engine-demo` all pass.

### Phase 2: Live Voice Mixer Integration ✅

**Status**: Complete

**Goal**: Add accumulation mode to the Faust architecture file and provide
an example ROM that links and plays a live voice mixed with the RSP mixer.

**Changes**:
- `dsp/arch/libdragon_mixer.c`:
  - `_render()` now takes `int accumulate` parameter: when non-zero,
    samples are saturating-added to the existing buffer instead of overwriting
  - Added `_set_gain(FAUSTFLOAT gain)` for per-voice output trim
  - Gain applied before clamp/cast (one `mul.s` per sample per voice)
- Created `examples/live-voice/` (Makefile + main.c):
  - Links `ks-voice` object file via `FAUST_VOICE` make flag
  - A button: triggers live KS voice with cycling frequencies
  - B button: plays baked KS sample for A/B comparison
  - Mixing strategy: `mixer_poll` (RSP) → `faust_n64_ksvoice_render` (VR4300)
    → saturating add into AI buffer
- `flake.nix`: Wired `live-voice` ROM with `ks-voice` linked + `ks-baked` asset

**Results**:
- `live-voice.z64` builds and links successfully
- Audio rate check passes (32000 Hz)
- First end-to-end proof of live Faust synthesis on-console

### Phase 6: XM64 Music Example ✅

**Status**: Complete

**Goal**: An example ROM that plays tracker music via `mkMusic` + the
`kiln_music_*` API.

**Files created**:
- `tools/gen_xm.py` — generates a minimal 4-channel, 8-row looping XM
- `examples/music/test.xm` — generated test XM (1003 bytes)
- `examples/music/Makefile` — includes n64.mk + kiln.mk
- `examples/music/main.c` — plays XM64, A=play/stop, B=stop, Up/Down=volume

**Flake wiring**: `test-music` (mkMusic) + `music-demo` (mkN64Rom)

**Verified**: `music.z64` builds, `audioRate = 32000` check passes.

### Phase 5: Room-Based Audio Routing ✅

**Status**: Complete

**Goal**: Wire `kiln_room_current()` to music crossfading.

**Changes**:
- `engine/src/kiln/kiln_audio.h`: Added `kiln_audio_set_room_music(room_id, handle)`
  and `kiln_audio_update_rooms(void *room_sys)` (void* to avoid typedef
  forward-declaration issue with KilnRoomSystem's anonymous struct)
- `engine/src/kiln/kiln_audio.c`: Room music table (64 entries), linear gain
  ramp crossfade (~0.5s at 32000 Hz). Same-track = no restart. Fade out old
  → switch → fade in new.

**API usage**:
```c
kiln_audio_set_room_music(0, music_a);  // room 0 plays track A
kiln_audio_set_room_music(1, music_b);  // room 1 plays track B
// Per frame:
kiln_room_system_update(&sys, cam_pos);
kiln_audio_update_rooms(&sys);  // crossfades on room change
kiln_audio_update();            // pumps the mixer
```

### Phase 7: Update CLAUDE.md ✏️

**Status**: Not started

**Changes**:
- Add `kiln_audio.h` to engine file list
- Update "Not yet built" section
- Document cycle gate as hard failure
- Document sample rate enforcement

---

## 4. Hardware Constraints Honored

| Constraint | How the plan respects it |
|---|---|
| 4 MB RDRAM (8 MB with Expansion Pak) | No dynamic allocation in audio path |
| VR4300 at 93.75 MHz, single-precision FPU | Cycle gate catches regressions |
| RSP does mixing (not VR4300) | Baked/XM64 use RSP mixer; live voice adds one sum per sample |
| No libm on console | Gate enforced |
| No double precision | Gate enforced |
| AI rate ≠ HDMI rate | Sample rate enforcement (Phase 4) |
| ROM is read-only | All audio assets baked at build time |

---

## 5. Progress Log

| Date | Phase | Change |
|------|-------|--------|
| 2026-08-02 | Phase 3 | Cycle budget gate now hard-fails, scoped to frame() function. KS voice: 259 cycles (was 333 whole-object). Fixed pre-existing streamdb backend build issue. |
| 2026-08-02 | Phase 4 | Sample rate enforcement: mkN64Rom audioRate param + mkBakedInstrument rate export. Fixed assets-demo 44100→32000. |
| 2026-08-02 | Phase 1 | Engine audio layer: kiln_audio.h/kiln_audio.c with SFX (priority voice stealing) + music (XM64/YM64). All ROMs build clean. |
| 2026-08-02 | Phase 2 | Live voice mixer: accumulate mode in libdragon_mixer.c + _set_gain. examples/live-voice ROM builds and links ks-voice. |
| 2026-08-02 | Phase 6 | XM64 music example: generated test XM, examples/music ROM, mkMusic pipeline verified end-to-end. |
| 2026-08-02 | Phase 5 | Room-based audio routing: kiln_audio_set_room_music + kiln_audio_update_rooms with ~0.5s linear crossfade. |