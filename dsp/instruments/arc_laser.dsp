// SPDX-License-Identifier: GPL-3.0-only
//
// arc_laser — the arcade zap: a naive square swept down in pitch.
//
// Space Invaders to Galaga: a square wave that starts high and falls fast, with
// a hard-edged amplitude. Naive, so it sparkles with aliasing on the way down,
// which on a real cabinet was just the sound of the sound chip.
//
//   freq  Hz, 150..1200 (where it ends)   gain  0..1   gate  rising edge fires

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 300, 150, 1200, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
f = freq * (1.0 + 5.0 * kl.decay(0.09, trg));

process = kl.npulse(f, 0.5) * kl.decay(0.22, trg) * (gain * 1);
