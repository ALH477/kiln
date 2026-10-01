// SPDX-License-Identifier: GPL-3.0-only
//
// dmx_switch — a wall switch: two quick mechanical clacks.
//
// The DMX sound library DOOM shipped with played 8-bit, 11 025 Hz samples: every
// effect has that bright, crunchy, grainy top and that ceiling at about 5 kHz.
// `kl.lofi(11025, 127)` is the whole of it: a sample-and-hold at 11 kHz and an
// 8-bit quantiser at the end of the chain. The sound itself is synthesised, not
// sampled.
//
//   gain  0..1   gate  rising edge flips it

import("stdfaust.lib");
kl = library("kiln.lib");

gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
second = trg : de.delay(8192, int(0.12 * ma.SR));
clack(t) = (no.noise : kl.bp(2200.0, 2.0)) * kl.decay(0.012, t) + kl.psin(kl.phasor_r(210.0, t)) * kl.decay(0.04, t) * 0.5;

process = (clack(trg) + 0.8 * clack(second)) : kl.lofi(11025.0, 127.0) : *(gain * 1.33);
