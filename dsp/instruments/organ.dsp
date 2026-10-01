// SPDX-License-Identifier: MIT
//
// organ — nine additive drawbars: a tonewheel organ.
//
// A tonewheel organ is an additive synthesiser: each drawbar is one sine, at a
// footage that sets its pitch relative to the key — 16' is an octave below, 5⅓'
// a fifth above that, 8' the note itself, and so on up to 1', three octaves
// above. They are sums of whole-number multiples of a single base frequency
// (the 16' tone), so ONE phasor drives all nine: the partial at multiple k is
// psin(k * phase). Nine oscillators for the price of one phasor and nine
// polynomials, and they can never drift out of tune with one another, which on
// the real instrument is a feature of the gearing and here is arithmetic.
//
// The key click on the attack is a short burst of the 3rd harmonic — the
// contact bounce of a real key — and what makes it feel played rather than
// generated.
//
//   freq  Hz, 65..1046     gain  0..1
//   low   16' and 5 1/3'   mid  8', 4', 2 2/3'    high  2', 1 3/5', 1 1/3', 1'
//   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 220, 65, 1046, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
low  = hslider("low", 0.5, 0, 1, 0.01);
mid  = hslider("mid", 1.0, 0, 1, 0.01);
high = hslider("high", 0.45, 0, 1, 0.01);
gate = button("gate");

// Partial multiples of the 16' base (freq/2): 16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1'.
env = kl.adsr(0.004, 0.02, 1.0, 0.035, gate);
p = kl.phasor(freq * 0.5);
part(k, a) = kl.psin(kl.wrap01(k * p)) * a;

bars = part(1, low) + part(3, low * 0.7) + part(2, mid) + part(4, mid * 0.9) + part(6, mid * 0.6)
     + part(8, high) + part(10, high * 0.7) + part(12, high * 0.6) + part(16, high * 0.5);
click = part(6, 1.0) * kl.decay(0.012, kl.edge(gate)) * 0.4;

process = (bars * 0.18 + click) * env : kl.chorus(1.5, 6.2) : *(gain * 2.2);
