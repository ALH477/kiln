// SPDX-License-Identifier: MPL-2.0
//
// pm_drone.dsp — PetaByte Madness' title and ambience bed.
//
// A pressure drone: what an underwater station sounds like from inside it
// at three in the morning, when the only things running are the pumps.
//
// ── Why baked, not live ────────────────────────────────────────────────
// Report Stage 1 (see CLAUDE.md, "Bake before you synthesise") wants 80-90%
// of a game's audio rendered offline into VADPCM and played back through
// the RSP mixer. A drone is the ideal candidate: it is long, it never has
// to respond to anything, and every cycle it would cost live on the VR4300
// is a cycle the demons want. mkBakedInstrument renders it at double
// precision on the host and audioconv64 encodes it, so the console pays a
// mixer channel and nothing else.
//
// ── The pulse is the gameplay clock ────────────────────────────────────
// docs/VEIL_DESIGN.md §7 specifies a `pulse` at F0/16 for the gameplay
// clock — the beat the ANTIPHON's window and the CARDINAL's halo run on.
// With F0 at 55 Hz that is ~3.4 Hz here, slow enough to read as breathing
// rather than as a tremolo, and it is phase-locked to the drone because it
// is derived from the same F0 rather than from its own oscillator. That
// lock is the point: anything on the CPU that reads this beat stays in
// sync with what the player is hearing, for free.
//
// ── No libm, deliberately ──────────────────────────────────────────────
// Nothing here reaches for tanh/exp/pow. That is not only for the live
// path's no-libm gate (this instrument is baked, so it would pass either
// way) — it is so the same source can be moved onto the live path later
// without being rewritten. The soft limiting at the end is a clipped
// cubic, which is a multiply and a compare rather than a transcendental.

import("stdfaust.lib");

// The fundamental. Low enough to feel structural rather than tonal; 55 Hz
// is A1, and the room tone of every machine space ever recorded sits near
// it because that is where mains hum and its neighbours live.
f0 = hslider("f0", 55, 20, 220, 0.01);
gain = hslider("gain", 0.35, 0, 1, 0.01);

// Three detuned partials. The detune is what stops it sounding like a test
// tone: at these spacings the beats between them are slow enough to hear
// as movement in the drone rather than as separate pitches.
partials = os.osc(f0) * 0.50
         + os.osc(f0 * 2.002) * 0.28
         + os.osc(f0 * 2.996) * 0.16;

// The pulse. F0/16, as the design doc specifies, mapped to a shallow
// amplitude sweep — 0.82 to 1.0, so it breathes without pumping.
pulse = 0.91 + 0.09 * os.osc(f0 / 16.0);

// Filtered noise standing in for water movement and ventilation. A
// one-pole lowpass, so the hiss sits under the drone instead of on top of
// it; anything brighter would fight the demons for the high frequencies
// the veil takes away from the world (VEIL_DESIGN.md §7).
wash = no.noise : fi.lowpass(1, 420) : _ * 0.10;

// Soft limit: a clipped cubic. Same curve shape as a tanh knee over the
// range that matters, for one multiply and a min/max.
soft(x) = k * (1.5 - 0.5 * k * k) with { k = max(-1.0, min(1.0, x)); };

mono = (partials * pulse + wash) * gain : soft;

// Two channels with a small delay on one. A few milliseconds of offset is
// enough to widen a drone across a stereo field without any of the phase
// weirdness a real reverb would cost, and it collapses harmlessly to mono
// on a console hooked up through a single speaker.
process = mono <: _, (_ : de.delay(512, 173));
