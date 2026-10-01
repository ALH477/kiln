// SPDX-License-Identifier: GPL-3.0-only
//
// marimba — a rosewood bar struck with a yarn mallet.
//
// A free bar's overtones are inharmonic (the second sits at 2.76 times the
// first); a marimba maker carves an arch under the bar to pull them to 1 : 4 :
// 10, which is why it sounds pitched and a plain metal bar does not. Those three
// ratios are the instrument. The fundamental rings; the 4th and 10th die in a
// fraction of the time, and that fast loss of the upper partials IS the "pock"
// of the attack.
//
//   freq  Hz, 130..2100   gain  0..1
//   hard  0 = yarn (dull)   1 = hard rubber (bright, clicky)
//   gate  rising edge strikes

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 262, 130, 2100, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
hard = hslider("hard", 0.4, 0, 1, 0.01);
gate = button("gate");

ex = kl.strike(700.0 + 7000.0 * hard, gate);

process = ex : kl.modes(freq, kl.tscale(freq), (1.0, 3.99, 9.92), (1.4, 0.32, 0.1), (1.0, 0.55, 0.28)) : *(gain * 0.62);
