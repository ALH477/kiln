// SPDX-License-Identifier: GPL-3.0-only
//
// bass_guitar — a fingered electric bass: a magnetic pickup on a steel string.
//
// Where `upright_bass` is a big acoustic body, an electric bass is nearly all
// string and pickup, so the loop here rings longer and brighter (less damping)
// and there is almost no body to colour it. The pickup is what shapes the
// tone: it senses string VELOCITY near the bridge, which tilts the spectrum up,
// modelled with a first-difference blend, and its own resonance sits around
// 3 kHz. `tone` is the knob on the guitar. A faint fret-noise tick rides the
// attack.
//
//   freq   Hz, 31..196   gain  0..1    decay  seconds
//   tone   0 = neck pickup, round   1 = bridge pickup, growl
//   gate   rising edge plucks

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 55, 31, 196, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 2.4, 0.3, 6, 0.01);
tone  = hslider("tone", 0.4, 0, 1, 0.01);
gate  = button("gate");

L = ma.SR / freq;
exc = kl.burst(freq, gate) : kl.lp1(2500.0) : (_ <: _, @(int(L / 4.0)) :> -);
tick = no.noise * kl.decay(0.01, kl.edge(gate)) : kl.bp(1800.0, 1.5) : *(0.05);
pickup(x) = x + tone * 1.4 * (x - x');

process = (exc : kl.string(freq, decay, 0.4) : pickup) + tick : kl.dcblock : kl.lp(3200.0, 1.2) : *(gain * 0.55);
