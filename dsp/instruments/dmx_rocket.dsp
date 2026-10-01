// SPDX-License-Identifier: GPL-3.0-only
//
// dmx_rocket — a rocket explosion: a deep rumble over a sub thump.
//
// The DMX sound library DOOM shipped with played 8-bit, 11 025 Hz samples: every
// effect has that bright, crunchy, grainy top and that ceiling at about 5 kHz.
// `kl.lofi(11025, 127)` is the whole of it: a sample-and-hold at 11 kHz and an
// 8-bit quantiser at the end of the chain. The sound itself is synthesised, not
// sampled.
//
//   gain  0..1   gate  rising edge detonates

import("stdfaust.lib");
kl = library("kiln.lib");

gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
sweep = kl.decay(0.6, trg);
rumble = no.noise : kl.lp(180.0 + 2400.0 * sweep * sweep, 0.9) : *(kl.decay(1.1, trg));
thump = kl.psin(kl.phasor_r(38.0 + 60.0 * kl.decay(0.12, trg), trg)) * kl.decay(0.6, trg);

process = (rumble * 1.5 + thump * 1.2) : kl.soft : kl.lofi(11025.0, 127.0) : *(gain * 1);
