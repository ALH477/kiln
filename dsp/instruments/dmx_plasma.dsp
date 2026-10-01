// SPDX-License-Identifier: GPL-3.0-only
//
// dmx_plasma — the plasma rifle: a falling zzap with a noisy skin.
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
f = 180.0 + 1900.0 * kl.decay(0.07, trg);
zap = (kl.phasor(f) * 2.0 - 1.0) * kl.decay(0.13, trg);
skin = no.noise : kl.bp(3500.0, 1.5) : *(kl.decay(0.06, trg) * 0.5);

process = (zap + skin) : kl.lofi(11025.0, 127.0) : *(gain * 0.903);
