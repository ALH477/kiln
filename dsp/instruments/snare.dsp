// SPDX-License-Identifier: GPL-3.0-only
//
// snare — a drum and a bed of wires.
//
// Two things sound when a snare is hit: the batter head (a short, pitched,
// slightly inharmonic thump — here two sines a fifth-and-a-bit apart) and the
// snare wires buzzing against the bottom head, which is band-limited noise that
// rings on well after the head has stopped. The balance between the two is the
// drum: `snap` is the wires. The tone's pitch falls a little at the start, as a
// struck membrane's does, and the noise is highpassed so the wires do not smear
// into the low end.
//
//   freq   Hz, 150..260 (the head)    gain  0..1
//   snap   0 = just the head   1 = mostly wires
//   decay  seconds the wires ring     gate  rising edge hits

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 185, 150, 260, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
snap  = hslider("snap", 0.6, 0, 1, 0.01);
decay = hslider("decay", 0.22, 0.05, 0.8, 0.001);
gate  = button("gate");

trg = kl.edge(gate);
bend = 1.0 + 0.25 * kl.decay(0.02, trg);
head = (kl.psin(kl.phasor_r(freq * bend, trg)) + 0.6 * kl.psin(kl.phasor_r(freq * 1.78 * bend, trg))) * kl.decay(0.1, trg);
wires = no.noise : kl.hp(1400.0, 0.8) : kl.lp(9000.0, 0.7) : *(kl.decay(decay, trg));

process = (head * (1.0 - 0.5 * snap) * 0.6 + wires * snap * 0.9) : kl.soft : *(gain * 1.0);
