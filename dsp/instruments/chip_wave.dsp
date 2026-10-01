// SPDX-License-Identifier: GPL-3.0-only
//
// chip_wave — a Game Boy wave channel: a 32-step, 4-bit wavetable.
//
// The DMG's third channel plays whatever 32 nibbles you load into wave RAM,
// which is why Game Boy music has such a recognisable "not-quite-a-synth" pad
// and bass: a hand-drawn waveform, sixteen levels, stepped. The three tables
// here (a bell-ish cycle, a ramp, a drawbar-organ sum) are `wave` 0, 1, 2. The
// table is a literal, so the whole voice is a table read: no filter, no
// multiply beyond the envelope.
//
//   freq  Hz, 65..1760   gain  0..1   wave  0, 1 or 2   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 262, 65, 1760, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
wave = hslider("wave", 0, 0, 2, 1);
gate = button("gate");

i = int(kl.phasor(freq) * 32.0);
w0 = rdtable(waveform{0.0666667,0.466667,0.733333,1,0.733333,0.466667,0.0666667,-0.2,-0.333333,-0.0666667,0.333333,0.6,0.333333,-0.0666667,-0.466667,-0.733333,-0.866667,-0.6,-0.2,0.0666667,-0.2,-0.6,-0.866667,-1,-0.733333,-0.333333,-0.0666667,0.0666667,-0.0666667,-0.2,-0.0666667,0.0666667}, i);
w1 = rdtable(waveform{-1,-1,-0.866667,-0.866667,-0.733333,-0.733333,-0.6,-0.6,-0.466667,-0.466667,-0.333333,-0.333333,-0.2,-0.2,-0.0666667,-0.0666667,0.0666667,0.0666667,0.2,0.2,0.333333,0.333333,0.466667,0.466667,0.6,0.6,0.733333,0.733333,0.866667,0.866667,1,1}, i);
w2 = rdtable(waveform{0.0666667,0.6,1,0.6,0.333333,0.733333,1,0.466667,0.0666667,0.6,0.866667,0.333333,-0.0666667,0.2,0.333333,-0.0666667,0.0666667,0.2,-0.2,-0.333333,-0.866667,-1,-0.6,-0.866667,-0.733333,-0.466667,-0.866667,-1,-0.6,-0.466667,-0.733333,-0.333333}, i);
env = kl.adsr(0.003, 0.1, 0.85, 0.06, gate) : kl.quant(3.0);

process = ba.selectn(3, int(wave), w0, w1, w2) * env * (gain * 1.5);
