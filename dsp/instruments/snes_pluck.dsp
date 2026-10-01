// SPDX-License-Identifier: MIT
//
// snes_pluck — a SNES plucked sample: a string, coarsely quantised and dulled.
//
// A short sampled pluck played back through the SNES's quantisation and its
// gaussian filter: the string loop of `pluck` at low brightness, an 5-bit grid
// and a 5 kHz roll-off.
//
//   freq  Hz, 65..1046   gain  0..1   gate  rising edge plucks

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 220, 65, 1046, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

exc = kl.burst(freq, gate) : kl.lp1(3500.0);

process = exc : kl.string(freq, 1.6, 0.6) : kl.dcblock : kl.quant(20.0) : kl.lp1(5000.0) : *(gain * 1.19);
