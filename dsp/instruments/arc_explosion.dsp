// SPDX-License-Identifier: GPL-3.0-only
//
// arc_explosion — one-bit noise whose clock falls: the arcade boom.
//
// Cabinet explosions are the noise generator with its clock swept down, which
// turns hiss into rumble over the length of the sound. No filter; the lowering
// clock is the filter.
//
//   gain  0..1   decay  seconds   gate  rising edge detonates

import("stdfaust.lib");
kl = library("kiln.lib");

gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 0.8, 0.2, 3, 0.001);
gate  = button("gate");

trg = kl.edge(gate);
rate = 700.0 + 11000.0 * kl.decay(decay * 0.5, trg);

process = kl.bitnoise(rate) * (kl.decay(decay, trg) : kl.quant(15.0)) * (gain * 1.07);
