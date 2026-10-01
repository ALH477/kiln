// SPDX-License-Identifier: GPL-3.0-only
//
// arc_siren — a two-tone alarm: a square flipping between two pitches.
//
// The warning of a hundred cabinets: one oscillator, a pitch that alternates a
// fifth every 350 ms. Held for as long as the danger lasts.
//
//   freq  Hz, 300..1200 (the low tone)   gain  0..1   gate  held = sounding

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 600, 300, 1200, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

f = freq * (1.0 + 0.5 * (kl.phasor(1.43) > 0.5));

process = kl.npulse(f, 0.5) * kl.lag(0.004, gate) * (gain * 1);
