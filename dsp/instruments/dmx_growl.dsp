// SPDX-License-Identifier: MIT
//
// dmx_growl — a monster growl: a low buzz through a moving vowel, torn by a 28 Hz tremolo.
//
// The DMX sound library DOOM shipped with played 8-bit, 11 025 Hz samples: every
// effect has that bright, crunchy, grainy top and that ceiling at about 5 kHz.
// `kl.lofi(11025, 127)` is the whole of it: a sample-and-hold at 11 kHz and an
// 8-bit quantiser at the end of the chain. The sound itself is synthesised, not
// sampled.
//
//   freq  Hz, 50..140   gain  0..1   gate  rising edge growls

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 75, 50, 140, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
vow = kl.decay(0.5, trg);
src = (kl.phasor(freq) * 2.0 - 1.0) + 0.3 * no.noise;
body = src <: (kl.bp(450.0 + 350.0 * vow, 5.0)), (kl.bp(900.0 + 400.0 * vow, 6.0) * 0.6) :> _;
trem = 0.65 + 0.35 * kl.tri(28.0);

process = body * trem * kl.decay(0.7, trg) : kl.lofi(11025.0, 127.0) : *(gain * 0.736);
