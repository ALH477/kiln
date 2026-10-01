// SPDX-License-Identifier: GPL-3.0-only
//
// laser — a falling zap: a band-limited saw whose pitch drops exponentially.
//
// The sound every arcade shooter shares: a bright tone that starts high and
// falls through about three octaves in a fifth of a second. The fall is
// exponential, not linear, because the ear hears pitch logarithmically and a
// linear sweep sounds as if it slows down. A second oscillator a fifth-and-a-bit
// away, swept the same way, thickens it; a highpass keeps it thin.
//
//   freq   Hz, 200..1500 (where the zap SETTLES)   gain  0..1
//   sweep  how far above `freq` it starts, in octaves-ish multiples (1..8)
//   decay  seconds     gate  rising edge fires

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 400, 200, 1500, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
sweep = hslider("sweep", 5, 1, 8, 0.01);
decay = hslider("decay", 0.22, 0.05, 0.8, 0.001);
gate  = button("gate");

trg = kl.edge(gate);
fall = kl.decay(decay * 0.55, trg);
f = freq * (1.0 + sweep * fall);
amp = kl.decay(decay, trg);

process = (kl.saw(f) + 0.6 * kl.saw(f * 1.51)) * 0.5 : kl.hp(250.0, 0.8) : *(amp * gain * 1.0) : kl.soft;
