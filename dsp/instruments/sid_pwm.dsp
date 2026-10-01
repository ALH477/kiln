// SPDX-License-Identifier: GPL-3.0-only
//
// sid_pwm — a C64 SID pulse voice with its width swept: the classic PWM lead.
//
// The SID's pulse width is a 12-bit register, and the thing every composer did
// with it was move it: a triangle sweep between thin and fat, so the harmonic
// content breathes without a filter. Here the width follows a 0.9 Hz triangle
// between 15% and 85%, and a one-pole lowpass stands in for the SID filter's
// gentle top roll-off. Naive pulse, so it has the SID's fizz too.
//
//   freq  Hz, 65..1046   gain  0..1   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 220, 65, 1046, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

w = 0.5 + 0.35 * kl.tri(0.9);
env = kl.adsr(0.004, 0.2, 0.75, 0.1, gate);

process = kl.npulse(freq, w) : kl.lp1(5500.0) : *(env * gain * 1.01);
