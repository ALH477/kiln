// SPDX-License-Identifier: MIT
//
// pluck — a plucked string: extended Karplus-Strong with a tuned loop.
//
// The library's reference live voice. One delay line, one allpass, one
// two-point lowpass and a burst of noise, so it is the cheapest thing here that
// still sounds like an instrument; every other string in the library starts
// from `kl.string`.
//
//   freq    Hz, 55..1760
//   gain    0..1
//   decay   seconds to -60 dB at the fundamental
//   bright  0 = a muted nylon thud, 1 = a steel string that rings
//   gate    rising edge plucks; the string rings out on its own

import("stdfaust.lib");
kl = library("kiln.lib");

freq   = hslider("freq", 220, 55, 1760, 0.01);
gain   = hslider("gain", 0.5, 0, 1, 0.01);
decay  = hslider("decay", 3.0, 0.2, 10, 0.01);
bright = hslider("bright", 0.5, 0, 1, 0.01);
gate   = button("gate");

exc = kl.burst(freq, gate) : kl.lp1(1500.0 + 9000.0 * bright);

process = exc : kl.string(freq, decay, 1.0 - bright) : kl.dcblock : *(gain * 1.0);
