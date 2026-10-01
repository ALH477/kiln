// SPDX-License-Identifier: GPL-3.0-only
//
// fm_bell — the 1 : 3.5 FM bell.
//
// Chowning's 1973 bell: a carrier modulated at a NON-integer multiple — here
// 3.5, plus a second pair at 7 over the same carrier — so the sidebands fall at
// frequencies that are not harmonics of anything, and what the ear gets is an
// inharmonic cluster that reads as struck metal. A decaying index makes the
// brilliance fade while the body rings, as in a real bell, and a long exponential
// amplitude envelope does the rest. Two operators and one envelope shape; it
// sits in the library as the cheap, bright counterpart to `bell`'s modal one.
//
//   freq   Hz, 110..1760   gain  0..1     decay  seconds
//   gate   rising edge strikes

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 440, 110, 1760, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 3.0, 0.3, 8, 0.01);
gate  = button("gate");

amp = kl.decay(decay, kl.edge(gate));
idx = kl.decay(decay * 0.5, kl.edge(gate));

m1 = kl.sine(freq * 3.5) * idx;
m2 = kl.sine(freq * 7.0) * idx * idx;
car = kl.sine_pm(freq, 0.19 * 2.4 * m1 + 0.19 * 1.0 * m2);

process = car * amp * (gain * 0.9);
