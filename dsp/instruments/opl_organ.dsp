// SPDX-License-Identifier: GPL-3.0-only
//
// opl_organ — a rock organ: a constant-index two-operator tone.
//
// The Yamaha OPL2 (YM3812) is the chip in the AdLib and the Sound Blaster, and
// so the sound of DOOM's music on a 1993 PC: every instrument is TWO sine
// operators, a modulator driving a carrier, with one operator of feedback and a
// choice of four waveforms (sine, half-sine, abs-sine, pulse-sine). `kl.opl2`
// is that voice; this file is its patch.
//
// A modulator an octave above the carrier at a fixed, low index adds the
// even harmonics a drawbar organ has. No index envelope at all: the tone does not
// change, only the gate.
//
//   freq  Hz, 65..1046   gain  0..1   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 262, 65, 1046, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

amp = kl.adsr(0.004, 0.02, 1.0, 0.04, gate);

process = kl.opl2(freq, 2.0, 1.0, 0.18, 0.45, 0, 0) * amp * (gain * 1);
