// SPDX-License-Identifier: GPL-3.0-only
//
// crash — a crash cymbal: metal oscillators and noise under a long decay.
//
// The same six-square metallic cluster as the hats (see hat_closed), a layer of
// highpassed noise for the wash, and an envelope with a two-stage shape: a
// loud, quick drop from the stick's impact, then a slow shimmering tail as the
// bell-edge modes take over. The tail brightens slightly as it falls because
// the highpass opens — a cymbal's low modes die first.
//
//   tune  0.6..1.6   gain  0..1    decay  seconds    gate  rising edge hits

import("stdfaust.lib");
kl = library("kiln.lib");

tune  = hslider("tune", 1, 0.6, 1.6, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 1.6, 0.5, 4.0, 0.001);
gate  = button("gate");

metal(t) = (kl.square(205.3 * t) + kl.square(304.4 * t) + kl.square(369.6 * t)
          + kl.square(522.7 * t) + kl.square(540.0 * t) + kl.square(800.0 * t)) * 0.16;

trg = kl.edge(gate);
env = 0.55 * kl.decay(0.12, trg) + 0.45 * kl.decay(decay, trg);
wash = no.noise * 0.3;

process = (metal(tune * 1.6) + wash) : kl.hp(4500.0 + 3000.0 * (1.0 - env), 0.8) : *(env * 1.4) : kl.soft : *(gain * 1.0);
