// SPDX-License-Identifier: GPL-3.0-only
//
// arc_jump — a platformer hop: a thin pulse rising about an octave.
//
//   freq  Hz, 150..700 (where it starts)   gain  0..1   gate  rising edge plays

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 260, 150, 700, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
f = freq * (1.0 + 1.6 * (1.0 - kl.decay(0.08, trg)));

process = kl.npulse(f, 0.25) * kl.decay(0.17, trg) * (gain * 1);
