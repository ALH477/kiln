// SPDX-License-Identifier: GPL-3.0-only
//
// harp — a gut-stringed harp: a soft pluck near the end of a long, bright
// string, with a soundboard.
//
// What separates it from `pluck` is the excitation and the long decay. A
// finger pulls a string aside and lets it go, which is a smooth triangular
// displacement, not a burst of noise; the comb `x - x@(L/6)` is the pluck
// position (a sixth of the way along, near the bridge, where harp strings are
// plucked), and it is what removes every sixth partial and gives the
// characteristic hollow-then-bright tone. The soundboard is two resonances
// where a harp's box actually sits, which is the difference between "a string"
// and "a harp" more than any of the string maths.
//
//   freq   Hz, 82..1760      gain  0..1      decay  seconds to -60 dB
//   gate   rising edge plucks

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 262, 82, 1760, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 4.0, 0.3, 12, 0.01);
gate  = button("gate");

L = ma.SR / freq;
// A triangular pulse one pluck-width wide, built from the gate's own timer.
w = max(2.0, L / 3.0);
age = (+(1.0) : *(1.0 - kl.edge(gate))) ~ _;
tri = max(0.0, 1.0 - abs(age * 2.0 / w - 1.0)) * (age < w) * (gate > 0.5 | age < w);
exc = (tri - tri@(int(L / 6.0))) : kl.lp1(4000.0);

board = _ <: _, (kl.bp(180.0, 6.0) * 0.6), (kl.bp(520.0, 5.0) * 0.4) :> _;

process = exc : kl.string(freq, decay, 0.25) : board : kl.dcblock : *(gain * 0.55);
