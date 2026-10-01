// SPDX-License-Identifier: MIT
//
// opl_lead — a reedy lead: a half-sine carrier.
//
// The Yamaha OPL2 (YM3812) is the chip in the AdLib and the Sound Blaster, and
// so the sound of DOOM's music on a 1993 PC: every instrument is TWO sine
// operators, a modulator driving a carrier, with one operator of feedback and a
// choice of four waveforms (sine, half-sine, abs-sine, pulse-sine). `kl.opl2`
// is that voice; this file is its patch.
//
// Choosing the half-sine waveform for the carrier (OPL2 waveform 1) gives the
// reedy, nasal tone of a lead patch; a modulator at three times the carrier and
// vibrato that fades in make it sing.
//
//   freq  Hz, 130..1320   gain  0..1   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 440, 130, 1320, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

amp = kl.adsr(0.01, 0.15, 0.8, 0.1, gate);
idx = 0.15 + 0.3 * kl.decay(0.25, kl.edge(gate));
f = freq * (1.0 + 0.006 * kl.tri(5.5) * kl.lag(0.3, gate));

process = kl.opl2(f, 3.0, 1.0, idx, 0.35, 0, 1) : kl.dcblock : *(amp * gain * 1.33);
