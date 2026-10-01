// SPDX-License-Identifier: MIT
//
// drone — three detuned sines and a breathing, filtered noise: a tonal bed.
//
// For a menu, a cave, a place where something is about to happen. Root, fifth
// and octave, each doubled a few cents apart so the pairs beat slowly against
// each other (0.2 to 0.7 Hz: a pulse the ear takes as the room breathing), under a
// lowpassed noise whose level moves on a 9-second LFO. No melody, no rhythm,
// nothing to get tired of. Meant to be held as long as the scene lasts.
//
//   freq  Hz, 32..220 (the ROOT)   gain  0..1    gate  held = sounding

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 55, 32, 220, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

env = kl.adsr(1.2, 0.5, 1.0, 1.5, gate);
pair(r, c) = kl.sine(freq * r * kl.cents(c)) + kl.sine(freq * r * kl.cents(-c));
tones = pair(1.0, 2.0) * 0.5 + pair(1.5, 3.0) * 0.28 + pair(2.0, 4.5) * 0.2;
air = no.noise : kl.lp(300.0, 0.7) : *(0.16 * (0.6 + 0.4 * kl.sine(0.11)));

process = (tones + air) * (env * gain * 0.8);
