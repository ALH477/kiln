<!-- SPDX-License-Identifier: MIT -->
# The Kiln instrument library

85 instruments, MIT-licensed, shipped with the engine: strings, keys, tuned
percussion, synths, **bass**, drums, sound effects, ambient beds and a **retro** family (see below). The table is
[`INDEX.md`](INDEX.md) (generated).

Each one is a Faust `.dsp` and is usable two ways, from the same source:

- **Baked** — rendered offline at full quality, VADPCM-encoded to a `.wav64`,
  played through the RSP mixer. Every instrument can be baked. This is the
  recommended path for most audio (see CLAUDE.md, "Bake before you synthesise").
- **Live** — compiled single-precision, one sample at a time, linked into the ROM
  and mixed by the VR4300. Only instruments marked `live` in `catalogue.json`;
  the build checks the claim.

```nix
# every baked instrument, one filesystem/
nix build .#instruments
# one, in your own flake
lib.mkBakedInstrument { name = "pluck"; src = ./dsp/instruments/pluck.dsp;
                        includes = [ ./dsp/lib ]; params = { freq = 220; }; gate = { param = "gate"; on = 0.0; off = 0.02; }; }
# the live voices, through the no-libm / no-double / cycle gates
nix build .#instrument-voices
```

## One control surface

Every instrument exposes `gain` (0..1, default 0.5 = about −6 dBFS) and `gate`
(a button). Pitched ones add `freq` in Hz; the rest are instrument-specific and
documented in the file's header (`hard`, `detune`, `decay`, `motor`, `vowel`...).
A `gate` rising edge strikes or plucks; for held instruments (pads, bass,
leads) the gate is the key. Percussion ignores release.

## The retro family: 8- and 16-bit, and cheap

38 voices that sound like old hardware and cost almost nothing to run. Most are
**live**, at 50–150 weighted cycles/sample (the clean voices are 200–600), so a
whole chip soundtrack fits in the audio budget of one orchestral voice:

- `chip_*`, `sid_pwm`, `pokey_buzz`, `arc_*` — NES pulse/triangle/noise, Game Boy
  wavetable, C64 arpeggio and PWM, Atari POKEY buzz, arcade zaps/coins/sirens.
  Deliberately **naive** (`kl.npulse`: one compare, no band-limiting): the
  aliasing is the timbre, so the catalogue marks them `aliasOk` and the checker
  skips its aliasing canary for them only.
- `opl_*` — the Yamaha OPL2 patches behind DOOM's music (two sine operators,
  feedback, four waveforms): the E1M1 metal guitar, slap bass, organ, brass,
  strings, lead, bell, pad. `kl.opl2` is the voice; each file is a patch.
- `dmx_*` — DOOM-style sound effects: synthesised, then put through an 11 025 Hz
  sample-and-hold and an 8-bit quantiser (`kl.lofi`), the DMX library's grain.
- `gen_*`, `snes_*` — Mega Drive FM bass and lead, SNES-style strings and pluck.

Four things to know: operator feedback past ~0.5 goes chaotic and period-doubles,
so the patches stay under it (the checker's pitch test is what found this); an
abs-sine *carrier* sounds an octave up, so `opl_pad` uses it as the modulator;
a perfect-fifth power chord's waveform repeats an octave below the root
(`opl_guitar` declares that alias); and the `dmx_*` effects that need long
delay lines (shotgun, door, switch) are baked-only, because a live voice's delay
line lives in RAM.

`tools/instruments/trim.py <name>` sets an instrument's output constant to the
library's −6 dBFS convention.

## Rules the build enforces

`nix flake check` runs `instruments-lint`, `-index`, `-render` and `-live`:

- every `.dsp` is in `catalogue.json` and vice versa; MIT SPDX line; standard
  controls present; a `live` source touches only libm-free namespaces;
- a **harmonic** instrument's measured pitch is within 3% of `freq` at the reference
  note *and* an octave up; an **inharmonic** one moves its whole spectrum with
  `freq`; gated ones release; no DC; no aliasing canary (energy above 0.4×SR);
- the default `gain` peaks between −14 and −1 dBFS, so levels mean the same
  everywhere; mono instruments render one channel (a stereo `.wav64` costs *two*
  mixer channels);
- each **live** voice is also rendered in its shipped form (single precision, one
  sample per call, gate changing on the exact sample) and held to the same numbers.

Loudness gates cannot tell a bell from a sine; these can. They are *measurements*,
not judgements of taste: **audition on hardware** (CLAUDE.md: do not trust
emulator audio).

## Writing one

1. `dsp/instruments/<name>.dsp`, first comment line `// <name> — <what it is>.`
   Build it from `kl = library("kiln.lib");` — **not** `os.*`, `fi.svf`/`fi.lowpass`,
   `ba.tau2pole`, `re.*`: they call libm in their constructors and a live voice
   fails to build. Baked-only instruments may use anything, but a `live: true`
   claim needs `kl.*`.
2. Add it to `catalogue.json` (family, kind, reference `freq`, `duration`, `gate`).
3. `tools/instruments/dev.sh <name>` — no Nix, no ROM. `--live` renders the shipped
   build; `--syms` lists libm calls; `--budget` is a proxy for the cycle gate.
4. `python3 tools/instruments/index.py dsp/instruments/catalogue.json dsp/instruments > dsp/instruments/INDEX.md`

## Known limits

- The piano has no string stiffness (its lowest octave is slightly too pure).
- The pads, drone and wind are one-shots, not seamless loops; hold them live or
  loop them in the mixer with a crossfade of your own.
- The MIPS cycle numbers in `catalogue.json` are *declared budgets*; the static
  estimate is not wall-clock. Profile with `TICKS`.
