// SPDX-License-Identifier: MIT
//
// supersaw — five detuned band-limited sawtooth oscillators through a lowpass
// that opens with the envelope: the trance/pop lead.
//
// Detuning is the entire trick. Five saws a few cents apart drift in and out of
// phase, so the sum has no stable waveform and the ear hears a chorus where
// there is no chorus. The oscillators are PolyBLEP, not naive saws: a naive saw
// folds its upper harmonics back below Nyquist as inharmonic hiss, and with five
// of them beating that hiss sits on top of every note. Baked only: five BLEP saws
// plus a filter is the price of one live voice several times over.
//
//   freq    Hz, 55..1760      gain  0..1
//   detune  spread, cents (each saw is at -1, -.5, 0, +.5, +1 of this)
//   bright  filter cutoff as a multiple of freq at full envelope
//   attack  seconds        release  seconds
//   gate    held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq    = hslider("freq", 220, 55, 1760, 0.01);
gain    = hslider("gain", 0.5, 0, 1, 0.01);
detune  = hslider("detune", 18, 0, 50, 0.1);
bright  = hslider("bright", 6, 1, 14, 0.1);
attack  = hslider("attack", 0.012, 0, 2, 0.001);
release = hslider("release", 0.35, 0.02, 3, 0.001);
gate    = button("gate");

env = kl.adsr(attack, 0.35, 0.8, release, gate);
saws = par(i, 5, kl.saw(freq * kl.cents((i - 2) * 0.5 * detune))) :> *(0.2);

process = saws : kl.lp(freq * bright * (0.3 + 0.7 * env), 0.9) : *(env * gain * 0.9) : kl.soft;
