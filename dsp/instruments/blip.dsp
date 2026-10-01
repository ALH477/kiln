// SPDX-License-Identifier: GPL-3.0-only
//
// blip — a UI beep: a short sine with a soft attack and release.
//
// The sound of a menu cursor moving. Its job is to be pitched, brief and
// unobtrusive, so the shape is a triangle-softened sine (a pure sine would
// click at its edges) with a three-millisecond attack and a fast decay that is
// over in under 80 ms. One instrument; `freq` makes it a whole interface
// vocabulary — rising pitches for "next", falling for "back", a low one for
// "denied".
//
//   freq  Hz, 300..3000    gain  0..1    gate  rising edge plays

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 880, 300, 3000, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
env = kl.decay(0.07, trg) : kl.lp1(120.0);

process = (kl.sine(freq) + 0.2 * kl.sine(freq * 2.0)) * env * (gain * 1.3);
