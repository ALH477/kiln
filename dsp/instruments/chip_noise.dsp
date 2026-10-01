// SPDX-License-Identifier: GPL-3.0-only
//
// chip_noise — the NES noise channel: one-bit noise at a chosen clock.
//
// A shift register clocked at a programmable rate and read one bit at a time:
// high clocks are hiss (hi-hats, cymbals), low clocks are rumble (kicks,
// explosions), and the whole drum kit of a Famicom soundtrack is this one
// channel with different `rate` and `decay` settings. Each hit is a step-quantised
// 4-bit decay.
//
//   rate   noise clock, Hz (300..16000)   gain  0..1   decay  seconds
//   gate   rising edge hits

import("stdfaust.lib");
kl = library("kiln.lib");

rate  = hslider("rate", 6000, 300, 16000, 1);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 0.14, 0.02, 1.5, 0.001);
gate  = button("gate");

process = kl.bitnoise(rate) * (kl.decay(decay, kl.edge(gate)) : kl.quant(15.0)) * (gain * 1.07);
