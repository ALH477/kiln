// SPDX-License-Identifier: MIT
//
// wind — filtered noise with a slowly wandering cutoff and gusts.
//
// A loopable ambient bed. Wind is noise; what makes it wind is its motion: a
// bandpass whose centre drifts between 300 and 900 Hz under two incommensurate
// LFOs (0.07 Hz and 0.113 Hz — they re-align only about every four minutes, so no
// loop of any length repeats a recognisable pattern), and an amplitude that rises
// and falls the same way. The second, higher bandpass is the whistle through
// cracks. Hold the gate for as long as the scene lasts.
//
//   gain   0..1    gust  0..1, how deep the swells go    gate  held = blowing

import("stdfaust.lib");
kl = library("kiln.lib");

gain = hslider("gain", 0.5, 0, 1, 0.01);
gust = hslider("gust", 0.6, 0, 1, 0.01);
gate = button("gate");

env = kl.adsr(0.8, 0.5, 1.0, 1.2, gate);
a = 0.5 + 0.5 * kl.sine(0.07);
b = 0.5 + 0.5 * kl.sine(0.113);
swell = 1.0 - gust * 0.7 * (1.0 - 0.5 * (a + b));

body = no.noise : kl.bp(300.0 + 600.0 * a, 1.1);
whistle = no.noise : kl.bp(1800.0 + 900.0 * b, 8.0) : *(0.35);

process = (body + whistle) * (env * swell * gain * 0.45) : kl.soft;
