// SPDX-License-Identifier: MIT
//
// kiln_jingle.dsp — the Kiln boot jingle.
//
// The Nintendo 64's boot sound is a short rising figure that resolves on a
// bright, wide chord — three seconds of "the machine is awake". This is
// the same gesture in a different mouth: a low swell that opens into a
// fifth and then a major triad, played on a soft synth rather than the
// original's sampled brass.
//
// ── Why baked, and why that is the whole point ─────────────────────────
// A boot jingle is the most bakeable audio a game has: it is short, fixed,
// and plays once before anything is on screen competing for the CPU.
// Report Stage 1 (CLAUDE.md, "Bake before you synthesise") wants exactly
// this rendered offline to VADPCM, and mkBakedInstrument's silence,
// over-quiet and clipping gates all apply — which matters more here than
// anywhere else, because this is the first thing anyone ever hears from
// the console and a clipped or inaudible boot reads as a broken cartridge.
//
// ── No libm ────────────────────────────────────────────────────────────
// Envelopes are built from ba.time and arithmetic rather than from exp,
// and the soft clip at the end is a cubic. Nothing here would trip the
// no-libm gate if it were ever moved onto the live path, which is the
// standing rule for every .dsp in this repo (see dsp/ks.dsp).

import("stdfaust.lib");

gain = hslider("gain", 0.5, 0, 1, 0.01);

// Seconds since the render started. ba.time is a sample counter, so this
// is the one place the sample rate appears — everything below is written
// in seconds and stays correct if the rate ever changes.
t = ba.time / ma.SR;

// A note that starts at `t0`, rises over `atk` and falls over `rel`.
// Piecewise linear rather than exponential: at three seconds nobody can
// tell, and it keeps the whole file free of transcendentals.
env(t0, atk, rel) = max(0.0, min(rise, fall))
with {
    rise = (t - t0) / atk;
    fall = 1.0 - (t - t0 - atk) / rel;
};

// Two detuned saws through a lowpass — a soft, wide synth voice rather
// than a bright one, so the chord blooms instead of stabbing.
voice(f, t0, atk, rel) =
    (os.sawtooth(f) + os.sawtooth(f * 1.006)) * 0.5
    : fi.lowpass(2, f * 6.0)
    : _ * env(t0, atk, rel);

// The figure. A1 alone, then the fifth, then the third and octave land
// together on the downbeat — the resolution is the moment the logo
// finishes assembling, and kiln_splash times the flash to it.
BOOT = 55.0;   // A1, same fundamental the ambience bed sits on

fig = voice(BOOT,          0.00, 0.18, 2.60) * 0.55   // root
    + voice(BOOT * 1.5,    0.45, 0.14, 2.15) * 0.42   // fifth
    + voice(BOOT * 2.0,    0.90, 0.10, 1.80) * 0.38   // octave
    + voice(BOOT * 2.5,    0.90, 0.10, 1.80) * 0.30   // major third above
    + voice(BOOT * 4.0,    1.25, 0.06, 1.40) * 0.20;  // the bright top

// A single soft strike on the downbeat: filtered noise with a fast decay,
// which is what gives the resolution an edge without a percussion sample.
hit = no.noise : fi.lowpass(2, 2400) : _ * env(1.25, 0.004, 0.55) * 0.28;

soft(x) = k * (1.5 - 0.5 * k * k) with { k = max(-1.0, min(1.0, x)); };

mono = (fig + hit) * gain : soft;

// Widened the same way the ambience bed is: a few milliseconds of delay on
// one side. Collapses harmlessly to mono on a single speaker.
process = mono <: _, (_ : de.delay(512, 211));
