// SPDX-License-Identifier: MIT
//
// opl_bass — a slap bass patch: a fast-falling index.
//
// The Yamaha OPL2 (YM3812) is the chip in the AdLib and the Sound Blaster, and
// so the sound of DOOM's music on a 1993 PC: every instrument is TWO sine
// operators, a modulator driving a carrier, with one operator of feedback and a
// choice of four waveforms (sine, half-sine, abs-sine, pulse-sine). `kl.opl2`
// is that voice; this file is its patch.
//
// An OPL2 bass is a sine carrier under a modulator whose level falls away within
// 100 ms: a hard, clanky pluck that settles into a round tone. Feedback gives the
// attack its bite.
//
//   freq  Hz, 33..260   gain  0..1   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 55, 33, 260, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

amp = kl.adsr(0.002, 0.7, 0.35, 0.08, gate);
idx = 0.1 + 0.7 * kl.decay(0.08, kl.edge(gate));

process = kl.opl2(freq, 1.0, 1.0, idx, 0.25, 0, 0) * amp * (gain * 1.02);
