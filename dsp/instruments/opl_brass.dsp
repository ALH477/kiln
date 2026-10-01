// SPDX-License-Identifier: GPL-3.0-only
//
// opl_brass — a brass section patch: the index swells with the note.
//
// The Yamaha OPL2 (YM3812) is the chip in the AdLib and the Sound Blaster, and
// so the sound of DOOM's music on a 1993 PC: every instrument is TWO sine
// operators, a modulator driving a carrier, with one operator of feedback and a
// choice of four waveforms (sine, half-sine, abs-sine, pulse-sine). `kl.opl2`
// is that voice; this file is its patch.
//
// Loudness and brightness rise together on a brass patch, so the modulator's
// envelope has the same slow attack as the carrier's, and feedback fattens it.
//
//   freq  Hz, 82..880   gain  0..1   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 233, 82, 880, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

amp = kl.adsr(0.05, 0.15, 0.85, 0.12, gate);
idx = 0.8 * kl.adsr(0.09, 0.2, 0.6, 0.12, gate);

process = kl.opl2(freq, 1.0, 1.0, idx, 0.2, 0, 0) : kl.dcblock : *(amp * gain * 0.824);
