// SPDX-License-Identifier: MIT
//
// arc_coin — the coin-up chirp: one square wave that jumps a fourth.
//
// One oscillator, two pitches: B5 for 70 ms, then up a perfect fourth, held and
// faded. It costs one voice and a comparison, which is why it is the universal
// pickup sound of every cabinet that had a cheap sound chip.
//
//   freq  Hz, 600..1600 (first note)   gain  0..1   gate  rising edge plays

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 988, 600, 1600, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
age = (+(1.0) : *(1.0 - trg) : min(1.0e6)) ~ _;
f = freq * (1.0 + 0.3348 * (age > 0.07 * ma.SR));

process = kl.npulse(f, 0.5) * kl.decay(0.4, trg) * (gain * 1);
