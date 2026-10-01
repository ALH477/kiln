// SPDX-License-Identifier: GPL-3.0-only
//
// sub_bass — a sine with a touch of its octave and a soft clip: the weight
// under a mix.
//
// The simplest instrument in the library and the cheapest live voice by a
// wide margin. The second harmonic is there for small speakers: a pure 40 Hz
// sine is inaudible on anything with a driver under two inches, and the ear
// reconstructs the pitch from the octave. The soft clip rounds the sum so
// loud notes thicken instead of getting louder.
//
//   freq  Hz, 28..220    gain  0..1
//   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq    = hslider("freq", 55, 28, 220, 0.01);
gain    = hslider("gain", 0.5, 0, 1, 0.01);
gate    = button("gate");

env = kl.adsr(0.004, 0.2, 0.9, 0.12, gate);
p = kl.phasor(freq);

process = (kl.psin(p) + 0.35 * kl.psin(kl.wrap01(2.0 * p))) * (env * gain * 0.85) : kl.soft;
