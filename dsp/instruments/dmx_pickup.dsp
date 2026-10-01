// SPDX-License-Identifier: GPL-3.0-only
//
// dmx_pickup — an item pickup: two quick rising blips.
//
// The DMX sound library DOOM shipped with played 8-bit, 11 025 Hz samples: every
// effect has that bright, crunchy, grainy top and that ceiling at about 5 kHz.
// `kl.lofi(11025, 127)` is the whole of it: a sample-and-hold at 11 kHz and an
// 8-bit quantiser at the end of the chain. The sound itself is synthesised, not
// sampled.
//
//   freq  Hz, 500..1500 (first blip)   gain  0..1   gate  rising edge plays

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 880, 500, 1500, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
age = (+(1.0) : *(1.0 - trg) : min(1.0e6)) ~ _;
f = freq * (1.0 + 0.5 * (age > 0.06 * ma.SR));

process = kl.sine(f) * kl.decay(0.25, trg) : kl.lofi(11025.0, 127.0) : *(gain * 1.01);
