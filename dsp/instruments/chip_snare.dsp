// SPDX-License-Identifier: MIT
//
// chip_snare — a console snare: bit noise over a short triangle.
//
//   gain  0..1   gate  rising edge hits

import("stdfaust.lib");
kl = library("kiln.lib");

gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
body = kl.tri4(190.0) * kl.decay(0.07, trg);

process = (kl.bitnoise(11000.0) * kl.decay(0.13, trg) + 0.6 * body) * (gain * 0.746);
