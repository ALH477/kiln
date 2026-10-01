// SPDX-License-Identifier: MIT
//
// piano — three detuned strings per note, struck by a felt hammer.
//
// Two things make a piano a piano and not a guitar. First, each note above the
// bass is THREE strings tuned a fraction of a cent apart, so their energy beats
// against itself and the decay has two slopes: a quick drop as the strings
// pull against each other, then a long quiet aftersound as the pair that
// happen to be in phase carry on. Here that is three `kl.string` loops at
// -0.9, 0 and +0.8 cents with slightly different decay times. Second, the
// excitation is a hammer, not a pluck: a half-sine pulse about a millisecond
// wide, narrower in the treble where the hammer is smaller and harder. A soft
// touch (`gain`) widens it and darkens the tone, as a real piano does.
//
// What this does NOT model is string stiffness (inharmonicity — real piano
// partials are sharp of the harmonic series, increasingly so at the bottom), and
// without it the lowest octave is a little too pure. It would take an allpass
// chain per string; it is the next thing to add. Baked only: three loops and
// a body.
//
//   freq   Hz, 65..2093     gain  0..1 (also the touch)
//   decay  seconds, scaled down for high notes     gate  rising edge strikes

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 262, 65, 2093, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 5.0, 0.5, 12, 0.01);
gate  = button("gate");

trg = kl.edge(gate);
age = (+(1.0) : *(1.0 - trg) : min(1.0e6)) ~ _;
// Hammer: half-sine, ~1.2 ms in the middle of the keyboard, wider when soft.
w = ma.SR * 0.0012 * (1.3 - 0.6 * gain) / (1.0 + freq * 0.0038);
hammer = (age < w) * kl.psin(age / (2.0 * w));
exc = hammer : kl.lp1(3500.0 + 9000.0 * gain);

t = decay * kl.tscale(freq);
strs = (exc : kl.string(freq * kl.cents(-0.9), t * 1.0, 0.3))
     + (exc : kl.string(freq, t * 0.85, 0.3))
     + (exc : kl.string(freq * kl.cents(0.8), t * 0.95, 0.3));

knock = no.noise * kl.decay(0.025, trg) : kl.lp1(500.0) : *(0.25);
board = _ <: _, (kl.bp(260.0, 2.0) * 0.5) :> _;

process = (strs * 0.12 + knock) : board : kl.dcblock : kl.lp(7500.0, 0.6) : *(gain * 1.3);
