// SPDX-License-Identifier: MIT
//
// wobble — a saw bass with its filter moved by an LFO: the dubstep wub.
//
// Two oscillators (saw and a narrow pulse an octave's worth of harmonics
// apart in character, not in pitch) go through a resonant lowpass whose cutoff
// swings between 120 Hz and 2.4 kHz at `rate`, in time with the music: 2 Hz is
// a half-time wub at 120 BPM, 4 Hz the classic. The swing is exponential-ish
// (the LFO is squared) because the ear hears cutoff logarithmically, and a
// linear sweep spends all its time sounding open. A sine sub keeps the weight
// constant while the filter moves above it.
//
//   freq   Hz, 30..130   gain  0..1
//   rate   LFO Hz (0.5..12)   depth  0..1   reso  1..10
//   gate   held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 55, 30, 130, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
rate  = hslider("rate", 3, 0.5, 12, 0.01);
depth = hslider("depth", 0.85, 0, 1, 0.01);
reso  = hslider("reso", 4, 1, 10, 0.1);
gate  = button("gate");

env = kl.adsr(0.006, 0.2, 0.95, 0.12, gate);
lfo = 0.5 + 0.5 * kl.sine(rate);
fc = 120.0 + 2300.0 * (1.0 - depth + depth * lfo * lfo);
osc = kl.saw(freq) * 0.55 + kl.pulse(freq * kl.cents(6.0), 0.35) * 0.4;

process = (osc : kl.lp(fc, reso)) + 0.45 * kl.sine(freq) : *(env * gain * 0.42) : kl.soft;
