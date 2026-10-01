// SPDX-License-Identifier: MIT
//
// vibraphone — aluminium bars over resonator tubes, with the motor.
//
// Same 1 : 4 : 10 tuning as the marimba but metal, so the partials ring for
// seconds instead of fractions of one, and a mallet strike leaves a pure bell
// with almost no "pock". The rotating discs in the tubes are the vibrato: not a
// pitch wobble, an AMPLITUDE one, because they open and close the tube. That is
// the `motor` — a sine on the output level, at 0 it is the "motor off" sound.
//
//   freq   Hz, 174..1400   gain  0..1
//   motor  tremolo rate, Hz (0 = off)   depth  0..1
//   gate   rising edge strikes

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 349, 174, 1400, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
motor = hslider("motor", 4.8, 0, 9, 0.01);
depth = hslider("depth", 0.35, 0, 1, 0.01);
gate  = button("gate");

ex = kl.strike(3500.0, gate);
trem = 1.0 - depth * (0.5 + 0.5 * kl.sine(motor));

process = ex : kl.modes(freq, kl.tscale(freq), (1.0, 4.0, 10.0), (5.5, 1.6, 0.4), (1.0, 0.5, 0.2)) : *(trem * gain * 0.8);
