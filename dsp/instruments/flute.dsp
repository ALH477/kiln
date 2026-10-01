// SPDX-License-Identifier: MIT
//
// flute — a sine with a thin overtone series and a layer of breath.
//
// A concert flute is close to the purest sound in the orchestra: a strong
// fundamental, a second harmonic around a third of its level, and the third
// barely there. What makes it a flute rather than an oscillator is the air —
// band-limited noise around the pitch, loudest at the onset (the "chiff") and
// settling to a thread — and a vibrato that the player does not begin until
// the note has been established, which is why it is ramped in from the gate.
//
//   freq    Hz, 262..2093    gain  0..1
//   breath  0..1             gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq   = hslider("freq", 523, 262, 2093, 0.01);
gain   = hslider("gain", 0.5, 0, 1, 0.01);
breath = hslider("breath", 0.4, 0, 1, 0.01);
gate   = button("gate");

env = kl.adsr(0.07, 0.1, 0.9, 0.12, gate);
vf  = freq * (1.0 + 0.005 * kl.sine(5.0) * kl.lag(0.7, gate));
p = kl.phasor(vf);
tone = kl.psin(p) + 0.3 * kl.psin(kl.wrap01(2.0 * p)) + 0.06 * kl.psin(kl.wrap01(3.0 * p));
air = no.noise : kl.bp(freq * 2.0, 1.5) : *(breath * (0.05 + 0.35 * kl.decay(0.09, kl.edge(gate))));

process = (tone * 0.55 + air) * (env * gain * 0.8) : kl.lp(7000.0, 0.7);
