// SPDX-License-Identifier: GPL-3.0-only
//
// pwm_pad — two pulse oscillators whose widths are moved by slow, unrelated
// LFOs, through a lowpass and a chorus: the late-'70s string-machine pad.
//
// A pulse wave's spectrum depends on its width, so sweeping the width sweeps
// the tone without a filter and without the pitch moving. Two oscillators at
// different LFO rates (0.31 Hz and 0.23 Hz) never repeat the same combination
// within any listening time, which is why it reads as alive rather than as a
// tremolo. Stereo: a Haas delay widens it; it collapses to mono harmlessly.
//
//   freq  Hz, 55..880    gain  0..1
//   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq    = hslider("freq", 220, 55, 880, 0.01);
gain    = hslider("gain", 0.5, 0, 1, 0.01);
attack  = hslider("attack", 0.5, 0.005, 4, 0.001);
release = hslider("release", 0.9, 0.05, 5, 0.001);
gate    = button("gate");

env = kl.adsr(attack, 0.5, 0.85, release, gate);
w1 = 0.5 + 0.3 * kl.sine(0.31);
w2 = 0.5 + 0.3 * kl.sine(0.23);
osc = (kl.pulse(freq * kl.cents(-4.0), w1) + kl.pulse(freq * kl.cents(4.0), w2)) * 0.45;

process = osc : kl.lp(min(freq * 5.0, 3500.0) * (0.4 + 0.6 * env), 0.7) : kl.chorus(4.0, 0.6) : *(env * gain * 1.35) : kl.haas(7.0);
