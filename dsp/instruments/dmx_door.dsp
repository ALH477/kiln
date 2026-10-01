// SPDX-License-Identifier: GPL-3.0-only
//
// dmx_door — a door: a bandpass sweep, and the clunk when it stops.
//
// The DMX sound library DOOM shipped with played 8-bit, 11 025 Hz samples: every
// effect has that bright, crunchy, grainy top and that ceiling at about 5 kHz.
// `kl.lofi(11025, 127)` is the whole of it: a sample-and-hold at 11 kHz and an
// 8-bit quantiser at the end of the chain. The sound itself is synthesised, not
// sampled.
//
//   gain  0..1   gate  rising edge opens

import("stdfaust.lib");
kl = library("kiln.lib");

gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
stop = trg : de.delay(32768, int(0.7 * ma.SR));
slide = no.noise : kl.bp(200.0 + 700.0 * (1.0 - kl.decay(0.5, trg)), 2.5) : *(kl.decay(0.8, trg) * 0.9);
clunk = (kl.psin(kl.phasor_r(70.0, stop)) + 0.5 * no.noise * kl.decay(0.02, stop)) * kl.decay(0.18, stop);

process = (slide * 1.3 + clunk) : kl.lofi(11025.0, 127.0) : *(gain * 1.02);
