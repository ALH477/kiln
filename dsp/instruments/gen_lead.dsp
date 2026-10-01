// SPDX-License-Identifier: MIT
//
// gen_lead — the Mega Drive lead: bright FM with a growing vibrato.
//
// A modulator at twice the carrier, an index that is bright at the attack and
// settles, and a vibrato that arrives late: the Sonic-era FM lead.
//
//   freq  Hz, 130..1320   gain  0..1   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 440, 130, 1320, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

amp = kl.adsr(0.006, 0.2, 0.8, 0.1, gate);
idx = 0.2 + 0.4 * kl.decay(0.3, kl.edge(gate));
f = freq * (1.0 + 0.007 * kl.tri(5.2) * kl.lag(0.4, gate));

process = kl.opl2(f, 2.0, 1.0, idx, 0.4, 0, 0) : kl.quant(64.0) : *(amp * gain * 1.03);
