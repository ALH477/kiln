// SPDX-License-Identifier: GPL-3.0-only
//
// choir — a sung vowel: a glottal-ish source through three formant filters.
//
// Voice is a source and a filter. The source here is a band-limited saw (a
// buzz rich in harmonics, like the glottal pulse train); the vocal tract is a
// set of resonances — FORMANTS — whose frequencies, not the pitch, are what
// make "ah" different from "ee". `vowel` morphs through a, e, i, o, u by
// interpolating the first three formants between the standard tabulated
// values (Peterson & Barney's averages for an adult). Everything else is life:
// vibrato that fades in after the onset, a breath of noise, and slow attack.
//
//   freq   Hz, 110..880    gain  0..1
//   vowel  0=a 1=e 2=i 3=o 4=u (fractions morph)
//   gate   held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 196, 110, 880, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
vowel = hslider("vowel", 0, 0, 4, 0.01);
gate  = button("gate");

// Formant tables, a e i o u. The index is a signal (it follows the slider), so
// the lookup is a runtime select, not ba.take, which wants a constant.
v  = kl.lag(0.05, vowel);
vi = min(3, int(v));
vf = v - vi;
pick(i, a, b, c, d, e) = ba.selectn(5, i, a, b, c, d, e);
fm(a, b, c, d, e) = pick(vi, a, b, c, d, e) * (1.0 - vf) + pick(vi + 1, a, b, c, d, e) * vf;
F1 = fm(800.0, 400.0, 270.0, 450.0, 325.0);
F2 = fm(1150.0, 1600.0, 2140.0, 800.0, 700.0);
F3 = fm(2900.0, 2700.0, 2950.0, 2830.0, 2700.0);

env = kl.adsr(0.18, 0.3, 0.9, 0.35, gate);
vib = 1.0 + 0.006 * kl.sine(5.3) * kl.lag(0.6, gate);
src = kl.saw(freq * vib) + 0.02 * no.noise;

process = src <: (kl.bp(F1, 9.0) * 1.0), (kl.bp(F2, 10.0) * 0.55), (kl.bp(F3, 12.0) * 0.3) :> *(env * gain * 0.25) : kl.soft;
