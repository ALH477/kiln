// SPDX-License-Identifier: MIT
//
// tom — a tuned tom-tom: a membrane's pitch fall, longer and higher than a kick.
//
// The same physics as `kick` with the tension higher: the head starts a fraction
// sharper and falls less far, and the shell rings longer. A little noise is the
// stick. Tuned by `freq`, so one instrument is a whole fill from floor tom
// (80 Hz) to rack (220 Hz).
//
//   freq   Hz, 70..260    gain  0..1    decay  seconds    gate  rising edge hits

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 110, 70, 260, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 0.5, 0.1, 1.5, 0.001);
gate  = button("gate");

trg = kl.edge(gate);
f = freq * (1.0 + 0.6 * kl.decay(0.05, trg));
body = kl.psin(kl.phasor_r(f, trg)) * kl.decay(decay, trg);
stick = no.noise * kl.decay(0.01, trg) : kl.bp(3200.0, 1.0);

process = (body * 1.3 + stick * 0.3) : kl.soft : *(gain * 1.0);
