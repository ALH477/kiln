// SPDX-License-Identifier: GPL-3.0-only
//
// cowbell — two square waves a fifth-ish apart, bandpassed.
//
// The 808 cowbell: 540 Hz and 800 Hz squares (a ratio of 1.48, close to but
// deliberately not a perfect fifth, which is what makes it clank rather than
// ring) through a bandpass near 2.6 kHz. The envelope has two parts, a hard
// strike and a longer ring at a lower level: a metal bell struck by a stick is
// loud for a few milliseconds and then settles to a quieter sustain.
//
//   tune   0.6..1.6    gain  0..1    decay  seconds    gate  rising edge hits

import("stdfaust.lib");
kl = library("kiln.lib");

tune  = hslider("tune", 1, 0.6, 1.6, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 0.35, 0.1, 1.0, 0.001);
gate  = button("gate");

trg = kl.edge(gate);
env = 0.7 * kl.decay(0.02, trg) + 0.3 * kl.decay(decay, trg);

process = (kl.square(540.0 * tune) + kl.square(800.0 * tune)) * 0.5 : kl.bp(2600.0 * tune, 1.4) : *(env * 2.4) : kl.soft : *(gain * 1.0);
