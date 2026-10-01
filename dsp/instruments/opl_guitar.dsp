// SPDX-License-Identifier: MIT
//
// opl_guitar — the overdriven metal guitar that opens E1M1.
//
// The Yamaha OPL2 (YM3812) is the chip in the AdLib and the Sound Blaster, and
// so the sound of DOOM's music on a 1993 PC: every instrument is TWO sine
// operators, a modulator driving a carrier, with one operator of feedback and a
// choice of four waveforms (sine, half-sine, abs-sine, pulse-sine). `kl.opl2`
// is that voice; this file is its patch.
//
// The recognisable DOOM sound is a GM "distortion guitar" patch on this chip: a
// modulator with heavy feedback (kept just below the point where an operator
// period-doubles and the note falls an octave), which turns it into a harmonic-
// rich buzzsaw, into a sine carrier, then hard clipped. A second voice a fifth
// above makes the power chord. `power` blends it in. (A perfect fifth's missing
// fundamental is an octave below the root, so the waveform repeats at half the
// root's rate: the catalogue accepts that alias.)
//
//   freq  Hz, 65..660   gain  0..1   power  0..1   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 82, 65, 660, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
power = hslider("power", 0.7, 0, 1, 0.01);
gate  = button("gate");

amp = kl.adsr(0.002, 0.5, 0.75, 0.08, gate);
v(f) = kl.opl2(f, 1.0, 1.0, 0.5, 0.4, 0, 0);
sig = v(freq) + power * 0.7 * v(freq * 1.4983);

process = sig : *(2.5) : kl.soft : kl.dcblock : kl.lp1(3800.0) : *(amp * gain * 0.76);
