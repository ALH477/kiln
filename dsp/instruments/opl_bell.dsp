// SPDX-License-Identifier: GPL-3.0-only
//
// opl_bell — a bell: a high odd modulator ratio and a long ring.
//
// The Yamaha OPL2 (YM3812) is the chip in the AdLib and the Sound Blaster, and
// so the sound of DOOM's music on a 1993 PC: every instrument is TWO sine
// operators, a modulator driving a carrier, with one operator of feedback and a
// choice of four waveforms (sine, half-sine, abs-sine, pulse-sine). `kl.opl2`
// is that voice; this file is its patch.
//
// A modulator at seven times the carrier throws sidebands well above the
// fundamental, and a slowly falling index lets them fade first, leaving the
// fundamental: the same shape as `fm_bell`, but quantised to what the chip's
// integer multipliers can reach.
//
//   freq  Hz, 130..1046   gain  0..1   gate  rising edge strikes

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 392, 130, 1046, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
idx = 0.6 * kl.decay(1.2, trg);

process = kl.opl2(freq, 7.0, 1.0, idx, 0.0, 0, 0) * kl.decay(2.2, trg) * (gain * 1);
