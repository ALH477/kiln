// SPDX-License-Identifier: GPL-3.0-only
//
// harpsichord — a quill plucking a bright string, and the jack's return click.
//
// No dynamics: a plectrum pulls the string aside to a fixed distance however
// hard the key is pressed, so a harpsichord is exactly as loud on every note and
// every touch. That is why `gain` here is a level, never a velocity, and why the
// tone does not change with it. The string rings briefly and brightly (a short
// decay, almost no loss filter), the pluck position sits a fifth of the way
// along (the comb in the excitation), and releasing the key drops a felt-
// tipped jack back onto the string with a small mechanical click — the sound a
// recording of one is recognisable by.
//
//   freq  Hz, 130..1760    gain  0..1    decay  seconds
//   gate  rising edge plucks; the falling edge is the damper

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 262, 130, 1760, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 1.8, 0.3, 5, 0.01);
gate  = button("gate");

L = ma.SR / freq;
exc = kl.burst(freq, gate) : kl.lp1(9000.0) : (_ <: _, @(int(L / 5.0)) :> -);

fall = (gate < 0.5) * (gate' > 0.5);
click = no.noise * kl.decay(0.012, fall) : kl.bp(2600.0, 2.0) : *(0.25);
// The damper: the string is muted when the key comes up.
damp = 1.0 - 0.97 * kl.lag(0.04, (gate < 0.5));

process = (exc : kl.string(freq, decay, 0.12)) * damp : +(click) : kl.dcblock : *(gain * 0.45);
