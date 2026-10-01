// SPDX-License-Identifier: GPL-3.0-only
//
// dmx_shotgun — a shotgun: a huge blast, then the pump.
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
pump = trg : de.delay(16384, int(0.42 * ma.SR));
blast = no.noise : kl.lp(2600.0, 0.7) : *(kl.decay(0.38, trg));
body = kl.psin(kl.phasor_r(90.0 * (1.0 + 1.0 * kl.decay(0.06, trg)), trg)) * kl.decay(0.25, trg);
ch = no.noise : kl.bp(1500.0, 1.2) : *(kl.decay(0.05, pump) + 0.6 * kl.decay(0.04, pump : de.delay(4096, int(0.1 * ma.SR))));

process = (blast * 1.3 + body + ch * 0.8) : kl.lofi(11025.0, 127.0) : *(gain * 0.696);
