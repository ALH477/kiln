// SPDX-License-Identifier: MIT
//
// guitar — an acoustic steel-string: a plucked loop through a guitar body.
//
// The string is `kl.string` driven by a noise burst that has been through a
// pick-position comb; the body is what makes it a guitar and not a pluck. A
// real top plate has a lowest "air" resonance near 100 Hz and a "top" mode
// near 200 Hz, then a broad shelf of damped modes above; three resonators and a
// gentle high shelf reproduce the part of that which the ear identifies.
//
//   freq   Hz, 82..1320    gain  0..1
//   decay  seconds to -60 dB at the fundamental
//   pick   0 = over the sound hole (warm)  1 = at the bridge (thin, nasal)
//   gate   rising edge strums one string

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 196, 82, 1320, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 2.4, 0.3, 8, 0.01);
pick  = hslider("pick", 0.35, 0, 1, 0.01);
gate  = button("gate");

L = ma.SR / freq;
// Pick position as a fraction of the string between 1/12 and 1/3.
pos = int(L * (0.33 - 0.25 * pick));
exc = kl.burst(freq, gate) : kl.lp1(6000.0) : (_ <: _, @(pos) :> -);

body = _ <: _, (kl.bp(98.0, 7.0) * 1.1), (kl.bp(205.0, 6.0) * 0.8), (kl.bp(430.0, 4.0) * 0.4) :> _;

process = exc : kl.string(freq, decay, 0.55) : body : kl.dcblock : *(gain * 0.58);
