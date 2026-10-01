// SPDX-License-Identifier: MIT
//
// rimshot — stick across the rim and head together: a crack.
//
// The sharp, dry, ringing "tock": two short high sines (the hoop and the head
// struck at once) over a click of bandpassed noise. All of it dies in under
// 60 ms, which is what makes it a rimshot and not a snare. `freq` tunes the
// hoop.
//
//   freq  Hz, 380..700    gain  0..1    gate  rising edge hits

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 480, 380, 700, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
tone = kl.psin(kl.phasor_r(freq, trg)) + 0.7 * kl.psin(kl.phasor_r(freq * 3.4, trg));
click = no.noise : kl.bp(2200.0, 1.2);

process = (tone * kl.decay(0.035, trg) * 0.6 + click * kl.decay(0.012, trg) * 0.8) : kl.soft : *(gain * 1.2);
