# Kiln Bass Synth

A 4-controller collaborative bass synthesizer ROM for N64 / ModRetro M64,
modeled on a Moog Subsequent 37 rather than a synth-action game.

A scripted intro plays at boot — one engine per bar, then two together — to
demo the system. Any button skips. The same voice allocator, mix, ADSR and
stick modulation that drive the intro also drive live performance, so the
intro *is* a real performance of the synth, not a separate audio asset. After
the intro, if every pad is left alone for 4 s, an attract tape plays a
bassline on port 1 (a DEMO badge shows); touching the pad takes it back.

## Controls

| Control                         | Action                                                    |
| ------------------------------- | --------------------------------------------------------- |
| **D-pad, C-buttons, L, B, A, R** | 12 notes: the player's scale, from their root upward     |
| **Z (held)**                    | Sustain: notes released while Z is down keep sounding     |
| **Analog stick X**              | Body brightness: picks the bright or dark table at note-on |
| **Analog stick Y**              | Sub drive: up = sub louder, body pans right               |
| **Stick magnitude**             | Vibrato depth                                             |
| **C-stick X**                   | ±1 semitone pitch bend                                    |
| **C-stick Y**                   | Mod wheel: 0..1 → vibrato depth                           |
| **Start** (pad 1)               | Open / close the patch editor — closing SAVES             |

The highest held button sounds. With legato on, moving to another button
glides there without re-attacking; with it off, each new button re-attacks
from the envelope's current level.

Button order, low to high: D-up, D-left, D-down, D-right, C-left, C-down,
C-right, C-up, L, B, A, R. In the chromatic scale that is one semitone each
from the player's root; the pentatonic and blues scales run their steps across
two octaves, so every button is a different note.

Z used to be both the twelfth note and the sustain modifier.

## Patch editor

Start opens a paged editor: one page per player (engine, octave, legato,
portamento, scale, volume) and a global page (master gain, LFO rate).
L/R or C-left/right changes page, D-up/down the row, D-left/right the value.
Start closes it and saves.

**Saving goes through `kiln_store`**, which takes the first backend that works:
the flashcart's SD card (`sd:/FORGE/BASSPAT.*`), then the 32 KB save chip
(`saveType = "sram256k"`), then read-only `rom:/`. The top-right of the status
line names the backend — teal when it can write, red when it cannot — and the
editor's footer and a toast after closing say what the last save or load
returned. Under an emulator this is the save chip; on the PC build it is
`rom:/`, read-only, in red.

This used to write `rom:/bass-synth.pat` with `fopen`: DragonFS is read-only, so
it never saved, silently. It also saved when the menu *opened*, before any
edit.

## Engines (4 layers)

Each engine is `(body_bright, body_dark, sub)` — three single-cycle
256-sample wavetables. A note plays the bright or the dark body table
(stick X at note-on) on one channel and the sub an octave below on another.

| Engine         | Body (bright)                 | Sub         |
| -------------- | ----------------------------- | ----------- |
| **HEAVY**      | sawtooth                      | low sine    |
| **SUB**        | sine + 3rd harmonic           | sine        |
| **GROWL**      | 2-op FM (carrier = mod freq)  | low sine    |
| **INDUSTRIAL** | square + filtered noise       | pure square |

Default per player: P1=HEAVY, P2=SUB, P3=GROWL, P4=INDUSTRIAL, octaves
+0/+12/+24/+36 from E1 (MIDI 28, 41 Hz). Octaves run −12..+36; notes are
clamped at E5 (MIDI 76).

## Per-engine ADSR

| Engine     | Attack | Decay  | Sustain | Release |
| ---------- | ------ | ------ | ------- | ------- |
| HEAVY      | 5 ms   | 180 ms | 0.7     | 120 ms  |
| SUB        | 8 ms   | 300 ms | 0.8     | 200 ms  |
| GROWL      | 4 ms   | 220 ms | 0.6     | 140 ms  |
| INDUSTRIAL | 3 ms   | 140 ms | 0.5     | 90 ms   |

Linear ramps, one multiply per stage per frame.

## Polyphony, pitch and level

- **6 simultaneous notes** × 2 channels each = **12 RSP mixer channels**.
- Steals a releasing note first, then the oldest.
- A body channel plays at `note Hz × 256`, because the table is one cycle of
  256 samples. libdragon's mixer asserts on any rate above a channel's limit,
  and the limit defaults to the 32 kHz output — about B1. The 12 channels'
  limits are raised to 192 kHz at boot; the highest reachable note with bend
  and vibrato is about 180 kHz.
- Portamento (0–500 ms, per player) slides the body rate towards its target.
- Level = envelope × engine gain × **player volume** × master, soft-clipped
  with `x / (1 + x)`.
- No libm on the hot path: note frequencies come from a table built by
  repeated multiplication, the LFO is `fm_sinf`, bend is linearised.

## On screen

- **Status line**: notes in use, master, LFO rate, and the save backend.
- **Scope**: the mixed output buffer, from `kiln_audio_set_tap` — what the RSP
  actually produced.
- **The arc**: 24 bars on a fixed sweep in front of a fixed camera, six per
  player in the player's colour. Bar 0 is the sub layer, bars 1–5 the body
  table's first five harmonics (from `tools/gen_bass_wav.py`'s recipes, bright
  and dark blended by stick X), scaled by the live envelopes and player volume
  and clamped to the frame. It is a model of the tables, not an FFT: the RSP
  does not hand per-channel samples back to the CPU.
- **Player columns**: engine, octave, volume, legato and scale, last note and
  envelope stage with a level bar, the stick, voices in use, bend and mod.

## Boot intro

`INTRO[]` is a flat list of `{t_ms, player, note, dur_ms}` events fired through
`note_on` / `note_off`. E minor, about 80 BPM:
- Bar 1 (0–1.5 s): P1 HEAVY walks E1–G1–A1–B1
- Bar 2 (1.7–3.1 s): P2 SUB rises E2–F2–G2–A2
- Bar 3 (3.2–5.0 s): P3 GROWL syncopated stabs
- Bar 4 (5.1–8.5 s): P4 INDUSTRIAL with P1 an octave up

## Build and look

```sh
nix build .#bass-synth
./dev shot bass-synth              # Ares capture
nix build .#bass-synth-patch       # boots into the editor, saves, reopens
nix build .#pc-bass-synth          # the host build (plays; save is read-only)
```

`bass-synth-patch` drives pad 1 from a tape: it turns P1's engine one step,
closes the editor (which saves) and reopens it, so a capture shows the save
backend and result with no controller.

Per CLAUDE.md, trust audio only on hardware — emulators get the AI clock
divider, RDRAM latency contention and FPU denormals subtly wrong.

## Files

- `main.c` — the ROM
- `Makefile` — standard `n64.mk + t3d.mk + kiln.mk` shape
- `../../tools/gen_bass_wav.py` — wavetable generator
- `../../assets/bass_wav/*.wav` — the 12 wavetables
- `../../nix/demos/bass-synth.nix` — the jump ROM and host builds

## Follow-ups (intentionally not here)

- **Per-channel filter biquad** (real subtractive filter) — per-sample VR4300
  DSP; out of scope per CLAUDE.md.
- **Effects** (chorus, delay, reverb) — the RSP mixer has none; would need
  custom RSP microcode.
- **A true bright/dark crossfade while a note is held** — 4 channels per note,
  so 3 notes instead of 6.
- **Polyphony > 6** — 2 more mixer channels per note, up to 16.
