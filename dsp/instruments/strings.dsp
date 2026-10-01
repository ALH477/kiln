// SPDX-License-Identifier: GPL-3.0-only
//
// strings — a string-section pad: three detuned saws, two ensemble delays and a
// slow bow.
//
// A section is many players, none exactly in tune and none exactly in time; the
// ensemble effect is that smear, reproduced by detuning three oscillators and
// running the sum through two slowly modulated delays at unrelated rates. The
// attack is slow because a section's bows do not land together. Baked only —
// stereo, three BLEP saws and two delay lines — and meant to be looped under a
// scene rather than triggered per note.
//
//   freq  Hz, 82..880    gain  0..1
//   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq    = hslider("freq", 220, 82, 880, 0.01);
gain    = hslider("gain", 0.5, 0, 1, 0.01);
attack  = hslider("attack", 0.28, 0.01, 3, 0.001);
release = hslider("release", 0.6, 0.05, 4, 0.001);
gate    = button("gate");

env = kl.adsr(attack, 0.4, 0.9, release, gate);
saws = (kl.saw(freq * kl.cents(-8.0)) + kl.saw(freq) + kl.saw(freq * kl.cents(8.0))) * 0.33;

process = saws : kl.lp(min(freq * 7.0, 5000.0), 0.6) : kl.chorus(5.0, 0.45) : kl.chorus(3.0, 0.71) : *(env * gain * 1.5) : kl.haas(9.0);
