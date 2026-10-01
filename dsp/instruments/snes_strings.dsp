// SPDX-License-Identifier: GPL-3.0-only
//
// snes_strings — a SNES string patch: detuned saws, 4-bit-ish, softened.
//
// The SNES played 4-bit BRR samples through a gaussian interpolator, so every
// instrument has a soft, slightly dull top and a faint graininess. Two detuned
// naive saws, a coarse quantiser and a one-pole lowpass land in the same place;
// the slow attack is the sampled-strings swell.
//
//   freq  Hz, 82..880   gain  0..1   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 220, 82, 880, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

amp = kl.adsr(0.2, 0.3, 0.9, 0.4, gate);
saws = (kl.phasor(freq * kl.cents(-7.0)) + kl.phasor(freq * kl.cents(7.0))) - 1.0;

process = saws : kl.quant(14.0) : kl.lp1(3200.0) : *(amp * gain * 1.2);
