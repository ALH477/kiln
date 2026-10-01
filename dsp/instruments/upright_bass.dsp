// SPDX-License-Identifier: GPL-3.0-only
//
// upright_bass — a plucked double-bass string and its big wooden body.
//
// A gut or steel string tuned in the lowest register, plucked with the pad of
// a finger: the excitation is soft (noise lowpassed hard, so there is no click)
// and the loop is damped strongly, because a bass string's high partials die
// almost at once and what remains is a round fundamental with a short "thud".
// The body is where the size comes from: a large air cavity resonating near 60 Hz
// and a top plate near 120 Hz, both strong, which is why a bass string
// stretched over a plank is thin and over a bass is enormous.
//
//   freq   Hz, 31..196   gain  0..1    decay  seconds
//   gate   rising edge plucks

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 55, 31, 196, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 1.6, 0.3, 5, 0.01);
gate  = button("gate");

exc = kl.burst(freq, gate) : kl.lp1(700.0);
body = _ <: _, (kl.bp(62.0, 5.0) * 0.8), (kl.bp(118.0, 4.0) * 0.6) :> _;

process = exc : kl.string(freq, decay, 0.7) : body : kl.dcblock : *(gain * 1.6);
