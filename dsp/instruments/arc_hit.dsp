// SPDX-License-Identifier: MIT
//
// arc_hit — an impact: a burst of noise over a falling 4-bit triangle thump.
//
//   freq  Hz, 60..300 (the thump)   gain  0..1   gate  rising edge hits

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 140, 60, 300, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
p = kl.phasor_r(freq * (1.0 + 2.0 * kl.decay(0.05, trg)), trg);
thump = (4.0 * abs(p - 0.5) - 1.0) : kl.quant(8.0);

process = (thump * kl.decay(0.15, trg) + 0.7 * kl.bitnoise(9000.0) * kl.decay(0.05, trg)) * (gain * 0.712);
