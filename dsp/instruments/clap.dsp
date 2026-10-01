// SPDX-License-Identifier: GPL-3.0-only
//
// clap — a handclap: several near-simultaneous noise bursts, then a tail.
//
// One hand does not hit the other once. A clap is three or four closely spaced
// impacts, a few milliseconds apart (nobody's hands land together), followed by
// the room's tail. Reproducing that spacing is what makes it a clap; a single
// noise burst through the same filter sounds like a snare's wires. The bursts
// are bandpassed around 1.2 kHz, where the flesh-on-flesh slap lives.
//
//   gain   0..1    decay  the tail, seconds    gate  rising edge claps

import("stdfaust.lib");
kl = library("kiln.lib");

gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 0.22, 0.05, 0.8, 0.001);
gate  = button("gate");

trg = kl.edge(gate);
ms(x) = int(x * 0.001 * ma.SR);
hit(d) = trg : de.delay(4096, ms(d)) : kl.decay(0.013, _);
tail = trg : de.delay(4096, ms(33.0)) : kl.decay(decay, _);
env = hit(0.0) + 0.9 * hit(10.0) + 0.8 * hit(21.0) + 0.9 * tail;

process = no.noise : kl.bp(1200.0, 1.6) : kl.hp(500.0, 0.7) : *(env * 3.0) : kl.soft : *(gain * 1.0);
