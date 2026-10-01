// SPDX-License-Identifier: MIT
//
// fm_bass — two-operator FM with a fast-falling index: the slap.
//
// The DX7's "E.Bass" family: a sine carrier modulated at the same frequency,
// with an index that starts high and collapses in about 50 ms. The attack is
// a hard, metallic, overtone-rich pluck and what is left is a clean round
// fundamental, which is what a slapped string does as its upper partials die
// first. A second, shorter pair at three times the carrier adds the "thwack"
// of the string hitting the fret. Cheap, and unlike a filtered saw it keeps
// its definition in the low register.
//
//   freq   Hz, 30..260   gain  0..1
//   snap   0..1, how hard the attack hits   decay  seconds of the body
//   gate   rising edge plucks; release damps it

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 55, 30, 260, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
snap  = hslider("snap", 0.6, 0, 1, 0.01);
decay = hslider("decay", 0.9, 0.2, 3, 0.01);
gate  = button("gate");

trg = kl.edge(gate);
amp = kl.adsr(0.002, decay, 0.0, 0.1, gate);
idx = kl.decay(0.05, trg);
thwack = kl.decay(0.03, trg);

body = kl.sine_pm(freq, 0.16 * (0.5 + 3.0 * snap) * idx * kl.sine(freq));
fret = kl.sine_pm(freq, 0.16 * 2.0 * snap * thwack * kl.sine(freq * 3.0)) * 0.25 * thwack;

process = (body * amp + fret) : kl.soft : *(gain * 1.0);
