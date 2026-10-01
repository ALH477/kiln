// SPDX-License-Identifier: GPL-3.0-only
//
// moog_bass — saw, square and sub through a plucking lowpass: the analogue
// mono bass.
//
// The "Minimoog bass" is a fixed recipe: a sawtooth and a square a touch
// apart, a sub-octave, and a lowpass filter whose cutoff is struck open by a
// fast envelope and falls back, so every note has a bright "bwow" at the front
// that settles into a round tone. That envelope, not the oscillators, is the
// character; `pluck` is how far it opens and `decay` how fast it closes. The
// filter is resonant but not screaming, and the sub is a plain sine: a
// second note an octave down would change the waveform's period and the note
// would read an octave low, so the sub sits at the SAME pitch.
//
//   freq   Hz, 30..260   gain  0..1
//   pluck  filter opening multiple (1..12)   decay  seconds
//   gate   held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 55, 30, 260, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
pluck = hslider("pluck", 7, 1, 12, 0.1);
decay = hslider("decay", 0.22, 0.05, 1.2, 0.001);
gate  = button("gate");

amp = kl.adsr(0.004, 0.25, 0.8, 0.1, gate);
sweep = kl.decay(decay, kl.edge(gate));
osc = kl.saw(freq) * 0.5 + kl.pulse(freq * kl.cents(5.0), 0.5) * 0.35 + kl.sine(freq) * 0.4;

process = osc : kl.lp(freq * (1.5 + pluck * sweep), 2.8) : *(amp * gain * 0.85) : kl.soft;
