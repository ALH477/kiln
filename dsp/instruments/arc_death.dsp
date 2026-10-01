// SPDX-License-Identifier: GPL-3.0-only
//
// arc_death — the player-lost wail: a falling pitch with a fast wobble.
//
//   freq  Hz, 200..800 (where it starts)   gain  0..1   gate  rising edge plays

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 500, 200, 800, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
f = freq * (0.15 + 0.85 * kl.decay(0.9, trg)) * (1.0 + 0.05 * kl.tri(13.0));

process = kl.npulse(f, 0.5) * kl.decay(1.1, trg) * (gain * 1);
