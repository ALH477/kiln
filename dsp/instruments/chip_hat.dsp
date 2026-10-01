// SPDX-License-Identifier: GPL-3.0-only
//
// chip_hat — a console hi-hat: the fastest noise clock, a very short decay.
//
//   gain  0..1   open  0 = closed, 1 = open   gate  rising edge hits

import("stdfaust.lib");
kl = library("kiln.lib");

gain = hslider("gain", 0.5, 0, 1, 0.01);
open = hslider("open", 0, 0, 1, 0.01);
gate = button("gate");

process = kl.bitnoise(16000.0) * kl.decay(0.025 + 0.3 * open, kl.edge(gate)) * (gain * 1.01);
