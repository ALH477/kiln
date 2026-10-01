// SPDX-License-Identifier: MIT
//
// powerup — a rising major arpeggio on a square wave.
//
// Five notes up a major chord (1, 5/4, 3/2, 2, 5/2), sixty milliseconds each,
// then a held top note that fades: the shape of "something good happened". The
// step counter runs off the gate edge, so the arpeggio always starts from the
// root regardless of when the gate falls, and the ratios are just-intonation
// fractions — a major triad built from 4:5:6 sounds sweeter than the equal-tempered
// one at this speed and register.
//
//   freq  Hz, 200..900 (the ROOT)   gain  0..1    gate  rising edge plays

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 392, 200, 900, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
age = (+(1.0) : *(1.0 - trg) : min(1.0e6)) ~ _;
step = min(4, int(age / (0.06 * ma.SR)));
ratio = ba.selectn(5, step, 1.0, 1.25, 1.5, 2.0, 2.5);
env = kl.decay(0.55, trg);

process = kl.square(freq * ratio) : kl.lp(6500.0, 0.7) : *(env * gain * 0.9);
