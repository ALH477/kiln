// SPDX-License-Identifier: MIT
//
// chip_kick — a console kick: a 4-bit triangle falling in pitch.
//
//   freq  Hz, 35..120   gain  0..1   gate  rising edge hits

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 55, 35, 120, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
p = kl.phasor_r(freq * (1.0 + 5.0 * kl.decay(0.045, trg)), trg);

process = ((4.0 * abs(p - 0.5) - 1.0) : kl.quant(8.0)) * kl.decay(0.22, trg) * (gain * 1.0);
