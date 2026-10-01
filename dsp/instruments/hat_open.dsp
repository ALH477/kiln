// SPDX-License-Identifier: MIT
//
// hat_open — the open hi-hat: the same six oscillators, left to ring.
//
// Identical source to `hat_closed`, a longer envelope and a lower highpass, so
// the shimmer has body. Closing the pedal is what chokes it; here `gate`
// RELEASE does that — drop the gate and the decay is cut to a short tail, so a
// closed hat played after it silences the open one as it does on a real kit.
//
//   tune   0.5..2     gain  0..1    decay  seconds
//   gate   rising edge opens; falling edge chokes

import("stdfaust.lib");
kl = library("kiln.lib");

tune  = hslider("tune", 1, 0.5, 2, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 0.4, 0.1, 1.5, 0.001);
gate  = button("gate");

metal(t) = (kl.square(205.3 * t) + kl.square(304.4 * t) + kl.square(369.6 * t)
          + kl.square(522.7 * t) + kl.square(540.0 * t) + kl.square(800.0 * t)) * 0.16;

trg = kl.edge(gate);
ring = kl.decay(decay, trg);
// Choke: while the gate is low the level is multiplied down fast.
choke = kl.lag(0.012, gate * 0.9 + 0.1);
sizzle = no.noise : kl.hp(5000.0, 0.7) : *(0.25);

process = (metal(tune) : kl.hp(5500.0, 1.0)) + sizzle : *(ring * choke * 1.3) : kl.soft : *(gain * 1.0);
