// SPDX-License-Identifier: GPL-3.0-only
//
// arc_powerup — a rising six-note arpeggio on a square wave.
//
//   freq  Hz, 200..900 (root)   gain  0..1   gate  rising edge plays

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 330, 200, 900, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
age = (+(1.0) : *(1.0 - trg) : min(1.0e6)) ~ _;
step = min(5, int(age / (0.05 * ma.SR)));
r = ba.selectn(6, step, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0);

process = kl.npulse(freq * r, 0.5) * kl.decay(0.6, trg) * (gain * 1);
