// SPDX-License-Identifier: GPL-3.0-only
//
// glockenspiel — small steel bars, struck hard.
//
// Untuned free-free beam: the overtones sit at 2.757, 5.404, 8.933 and 13.34
// times the fundamental (the roots of the Euler-Bernoulli beam equation), and
// nobody carves the bar to move them. Played in the top octaves the upper
// partials are above hearing or above Nyquist and the note reads as nearly
// pure; played low, the clash of those ratios is what makes it glitter.
// Partials at or above 0.45*SR are muted by kl.mode, not aliased.
//
//   freq  Hz, 523..2100    gain  0..1
//   gate  rising edge strikes

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 1047, 523, 2100, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

ex = kl.strike(9000.0, gate);

process = ex : kl.modes(freq, kl.tscale(freq), (1.0, 2.757, 5.404, 8.933, 13.344), (3.0, 1.4, 0.7, 0.35, 0.18), (1.0, 0.55, 0.4, 0.25, 0.15)) : *(gain * 0.5);
