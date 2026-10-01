// SPDX-License-Identifier: GPL-3.0-only
//
// dmx_pistol — a pistol shot: a crack and a short body.
//
// The DMX sound library DOOM shipped with played 8-bit, 11 025 Hz samples: every
// effect has that bright, crunchy, grainy top and that ceiling at about 5 kHz.
// `kl.lofi(11025, 127)` is the whole of it: a sample-and-hold at 11 kHz and an
// 8-bit quantiser at the end of the chain. The sound itself is synthesised, not
// sampled.
//
//   gain  0..1   gate  rising edge fires

import("stdfaust.lib");
kl = library("kiln.lib");

gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
crack = no.noise * kl.decay(0.07, trg) : kl.bp(1800.0, 0.8);
body = kl.psin(kl.phasor_r(150.0 * (1.0 + 1.5 * kl.decay(0.04, trg)), trg)) * kl.decay(0.1, trg);

process = (crack * 1.4 + body * 0.9) : kl.lofi(11025.0, 127.0) : *(gain * 0.791);
