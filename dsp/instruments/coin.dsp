// SPDX-License-Identifier: MIT
//
// coin — the two-note pickup chirp: a short note, then a higher one that rings.
//
// Two squares a perfect fourth apart (ratio 4/3), the second starting 70 ms after
// the first and ringing about five times as long, is the shape of "something was
// collected" in more games than there is room to list. A square, because it is
// the one waveform with a clean 50% duty and a hollow, definite pitch that cuts
// through a busy mix at very low level.
//
//   freq  Hz, 600..1600 (the FIRST note)   gain  0..1    gate  rising edge plays

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 988, 600, 1600, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
age = (+(1.0) : *(1.0 - trg) : min(1.0e6)) ~ _;
// First note: a 70 ms window, with its edges smoothed so it does not click.
first = (age < 0.07 * ma.SR) : kl.lag(0.004);
second = trg : de.delay(4096, int(0.07 * ma.SR));
n1 = kl.square(freq) * first * 0.7;
n2 = kl.square(freq * 1.3348) * kl.decay(0.38, second);

process = (n1 + n2) : kl.lp(7000.0, 0.7) : *(gain * 0.8);
