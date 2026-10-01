// SPDX-License-Identifier: MIT
//
// chip_arp — a C64-style arpeggio: a chord faked by cycling its notes at 50 Hz.
//
// The SID had three voices, and a tune with chords and a bass line and a lead
// had none left, so composers played the chord's notes one after another so fast
// the ear fuses them into one shimmering chord. The step is one PAL frame (20 ms);
// the cycle is three notes, 16.7 times a second. `chord` is major, minor,
// diminished or suspended. Built on `npulse`, so it costs about one voice.
//
//   freq  Hz, 65..1046 (the root)   gain  0..1   chord  0 maj, 1 min, 2 dim, 3 sus4
//   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 262, 65, 1046, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
chord = hslider("chord", 0, 0, 3, 1);
gate  = button("gate");

c = int(chord);
r2 = ba.selectn(4, c, 1.2599, 1.1892, 1.1892, 1.3348);
r3 = ba.selectn(4, c, 1.4983, 1.4983, 1.4142, 1.4983);
step = int(kl.phasor(16.667) * 3.0);
ratio = ba.selectn(3, step, 1.0, r2, r3);
env = kl.adsr(0.002, 0.1, 0.8, 0.05, gate);

process = kl.npulse(freq * ratio, 0.5) * env * (gain * 1.01);
