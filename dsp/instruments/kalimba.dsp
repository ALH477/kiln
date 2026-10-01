// SPDX-License-Identifier: GPL-3.0-only
//
// kalimba — steel tines on a hollow wooden box (the "thumb piano").
//
// A tine is a CANTILEVER, not a free bar: its overtones sit at 6.267 and 17.55
// times the fundamental, a much wider gap than a bar's, which is why the note
// is almost pure with a short "tick" on top. The box adds the hollow thump a
// bare tine does not have — a low resonance excited by the same pluck, plus a
// breath of lowpassed noise for the thumbnail scrape.
//
//   freq  Hz, 262..1400   gain  0..1
//   gate  rising edge plucks

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 523, 262, 1400, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

ex = kl.strike(6000.0, gate);
scrape = no.noise * kl.decay(0.04, kl.edge(gate)) : kl.lp1(1800.0) : *(0.08);
tine = ex : kl.modes(freq, kl.tscale(freq), (1.0, 6.267, 17.55), (2.2, 0.28, 0.07), (1.0, 0.32, 0.1));
box = ex : kl.modes(190.0, 1.0, (1.0, 1.9), (0.22, 0.1), (0.5, 0.2));

process = (tine + box + scrape) * (gain * 0.6);
