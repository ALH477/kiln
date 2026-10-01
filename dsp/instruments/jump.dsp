// SPDX-License-Identifier: MIT
//
// jump — a rising chirp: a pulse wave swept upward and shut off.
//
// A platformer's jump is a pitch that rises about an octave and a half in a
// tenth of a second, fast at first, with the amplitude falling away at the top.
// The duty cycle is 25%, narrower than the square used for `coin`, because that
// reads thinner and "springier" and does not compete with the coin's hollow
// tone when both sound at once.
//
//   freq  Hz, 150..700 (where the jump STARTS)   gain  0..1
//   gate  rising edge plays

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 260, 150, 700, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

trg = kl.edge(gate);
rise = 1.0 - kl.decay(0.09, trg);
f = freq * (1.0 + 1.7 * rise);

process = kl.pulse(f, 0.25) : kl.lp(6000.0, 0.7) : *(kl.decay(0.16, trg) * gain * 0.9);
