// SPDX-License-Identifier: GPL-3.0-only
//
// opl_strings — a string pad: two detuned voices, slow attack.
//
// The Yamaha OPL2 (YM3812) is the chip in the AdLib and the Sound Blaster, and
// so the sound of DOOM's music on a 1993 PC: every instrument is TWO sine
// operators, a modulator driving a carrier, with one operator of feedback and a
// choice of four waveforms (sine, half-sine, abs-sine, pulse-sine). `kl.opl2`
// is that voice; this file is its patch.
//
// Two copies of a feedback-saw patch six cents apart give the ensemble beat; the
// slow bow comes from the attack. Twice the cost of the other patches.
//
//   freq  Hz, 82..660   gain  0..1   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 220, 82, 660, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

amp = kl.adsr(0.22, 0.3, 0.9, 0.4, gate);
v(f) = kl.opl2(f, 1.0, 1.0, 0.15, 0.2, 0, 0);

process = (v(freq * kl.cents(-3.0)) + v(freq * kl.cents(3.0))) : kl.dcblock : *(amp * gain * 0.507);
