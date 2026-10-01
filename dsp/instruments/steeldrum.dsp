// SPDX-License-Identifier: GPL-3.0-only
//
// steeldrum — a tenor pan: a tuned note pocket on a hammered steel pan.
//
// The maker's whole craft is tuning each note's overtones into harmonic
// relationship with it — an octave, a twelfth, a double octave — so the pan is
// the one tuned-percussion family where the partials ARE harmonic and the
// note is pitched. The hammered, slightly "off" quality that remains comes
// from each partial being doubled a few cents apart (the beating) and the
// short noisy "tink" of the stick on steel.
//
//   freq  Hz, 262..1175   gain  0..1
//   gate  rising edge strikes

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 440, 262, 1175, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

ex = kl.strike(5000.0, gate);
tink = no.noise * kl.decay(0.025, kl.edge(gate)) : kl.hp(2500.0, 1.0) : *(0.12);

rs = (1.0, 2.0, 3.0, 4.02, 6.1);
ts = (1.6, 1.1, 0.7, 0.4, 0.2);
as = (1.0, 0.7, 0.4, 0.22, 0.1);

process = (ex <: kl.modes(freq, kl.tscale(freq), rs, ts, as), kl.modes(freq * kl.cents(5.0), kl.tscale(freq), rs, ts, as) :> *(0.5)) + tink : *(gain * 0.5);
