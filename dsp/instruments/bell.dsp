// SPDX-License-Identifier: MIT
//
// bell — a cast bell, built from its measured partial series.
//
// A bell does not have a fundamental in the way a string does; it has a
// STRIKE NOTE the ear invents from a set of partials near 2 : 2.4 : 3 : 4
// times a lower "hum". The series below (hum, prime, tierce, quint, nominal,
// and the rest) is the one acousticians report for a church bell, with the
// minor third between the prime and the tierce that gives a bell its sadness.
// `freq` is the PRIME. Each partial is doubled, one slightly sharp, and the
// pair beating against each other is the slow shimmer of a real bell — a
// single set of modes sounds like a synth.
//
//   freq  Hz, 110..880    gain  0..1
//   hard  clapper hardness: 0 = muffled  1 = bright
//   gate  rising edge strikes

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 220, 110, 880, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
hard = hslider("hard", 0.6, 0, 1, 0.01);
gate = button("gate");

ex = kl.strike(1500.0 + 8000.0 * hard, gate);

rs = (0.5, 1.0, 1.183, 1.506, 2.0, 2.514, 2.662, 3.011, 4.166, 5.433, 6.796, 8.215);
ts = (9.0, 8.0, 5.5, 4.5, 4.0, 2.8, 2.4, 2.0, 1.3, 0.9, 0.6, 0.4);
as = (0.55, 1.0, 0.75, 0.5, 0.6, 0.32, 0.3, 0.26, 0.2, 0.14, 0.1, 0.07);

process = ex <: kl.modes(freq, kl.tscale(freq), rs, ts, as), kl.modes(freq * 1.0035, kl.tscale(freq), rs, ts, as) :> *(gain * 0.15);
