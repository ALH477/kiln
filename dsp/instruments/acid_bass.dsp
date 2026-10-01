// SPDX-License-Identifier: MIT
//
// acid_bass — a resonant lowpass swept by a decaying envelope over a sawtooth:
// the squelch.
//
// The sound is the filter's resonance riding the envelope. At high `reso` the
// state-variable filter nearly self-oscillates at the cutoff, so as the cutoff
// falls the peak sweeps down through the harmonics, each one briefly
// emphasised: the "wow" that gives the genre its name. `envmod` is how far the
// cutoff travels; `decay` how long it takes. A tanh-ish clip after the filter
// keeps the resonance from running away on loud notes the way the analogue
// circuit's transistors did.
//
//   freq    Hz, 33..440     gain  0..1
//   cutoff  resting cutoff, Hz     reso  1..16     envmod  0..8
//   decay   seconds         gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq   = hslider("freq", 55, 33, 440, 0.01);
gain   = hslider("gain", 0.5, 0, 1, 0.01);
cutoff = hslider("cutoff", 300, 80, 2000, 1);
reso   = hslider("reso", 8, 1, 16, 0.1);
envmod = hslider("envmod", 4, 0, 8, 0.01);
decay  = hslider("decay", 0.28, 0.04, 1.5, 0.001);
gate   = button("gate");

amp = kl.adsr(0.002, 0.1, 0.9, 0.06, gate);
sweep = kl.decay(decay, kl.edge(gate));
fc = cutoff * (1.0 + envmod * sweep);

process = kl.saw(freq) : kl.lp(fc, reso) : kl.tnh : *(amp * gain * 0.9);
