// SPDX-License-Identifier: GPL-3.0-only
//
// opl_pad — a slow pad: an abs-sine carrier.
//
// The Yamaha OPL2 (YM3812) is the chip in the AdLib and the Sound Blaster, and
// so the sound of DOOM's music on a 1993 PC: every instrument is TWO sine
// operators, a modulator driving a carrier, with one operator of feedback and a
// choice of four waveforms (sine, half-sine, abs-sine, pulse-sine). `kl.opl2`
// is that voice; this file is its patch.
//
// An abs-sine MODULATOR (OPL2 waveform 2: its period is half a cycle, so it
// drives the carrier with only even harmonics) gives the glassy, hollow tone of a
// pad patch. (As a CARRIER it would sound an octave up.)
//
//   freq  Hz, 82..660   gain  0..1   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 220, 82, 660, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

amp = kl.adsr(0.35, 0.3, 0.9, 0.5, gate);

process = kl.opl2(freq, 1.0, 1.0, 0.22, 0.3, 2, 0) : kl.dcblock : *(amp * gain * 0.925);
