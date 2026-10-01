// SPDX-License-Identifier: GPL-3.0-only
//
// gen_bass — the Mega Drive slap bass: FM with feedback, crunched.
//
// The YM2612 in the Mega Drive is a four-operator FM chip with a 9-bit DAC, and
// its signature bass is a slap: feedback-saw attack, a quickly falling index, a
// clean fundamental, all coarsely quantised. Two operators with feedback and a
// 7-bit output stage are the same shape.
//
//   freq  Hz, 33..260   gain  0..1   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 55, 33, 260, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

amp = kl.adsr(0.002, 0.6, 0.45, 0.07, gate);
idx = 0.12 + 0.9 * kl.decay(0.06, kl.edge(gate));

process = kl.opl2(freq, 1.0, 1.0, idx, 0.3, 0, 0) : kl.quant(64.0) : *(amp * gain * 1.03);
