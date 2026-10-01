// SPDX-License-Identifier: GPL-3.0-only
//
// explosion — noise through a falling lowpass, over a sub thump.
//
// Three parts, because an explosion is three events: the crack (a burst of
// bright noise in the first 20 ms), the body (noise through a lowpass whose
// cutoff falls from several kilohertz to a few hundred over a second — the
// energy moving down the spectrum as the fireball expands and cools) and the
// thump (a sine falling from 90 Hz to 30, felt more than heard). Without the
// sweep it is white noise with a volume envelope; with it, it is a place.
//
//   gain   0..1    decay  seconds    gate  rising edge detonates

import("stdfaust.lib");
kl = library("kiln.lib");

gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 1.3, 0.3, 4, 0.001);
gate  = button("gate");

trg = kl.edge(gate);
sweep = kl.decay(decay * 0.7, trg);
body = no.noise : kl.lp(220.0 + 5200.0 * sweep * sweep, 0.9) : *(kl.decay(decay, trg));
crack = no.noise * kl.decay(0.02, trg) : kl.hp(1500.0, 0.8);
thump = kl.psin(kl.phasor_r(30.0 + 60.0 * kl.decay(0.15, trg), trg)) * kl.decay(decay * 0.5, trg);

process = (body * 1.4 + crack * 0.7 + thump * 1.2) : kl.soft : *(gain * 1.0);
