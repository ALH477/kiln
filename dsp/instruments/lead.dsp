// SPDX-License-Identifier: GPL-3.0-only
//
// lead — a monophonic saw-and-pulse lead with glide and a late vibrato.
//
// The classic one-voice melody instrument. A saw and a narrow pulse seven cents
// away fill out the sound (an octave-down sub would be tempting and wrong: it
// halves the waveform's period, and the note then reads an octave low). `glide`
// lags the pitch so a legato line slides between notes instead of stepping; the
// vibrato fades in 300 ms after the note starts, because a held note is where a
// player adds it. The lowpass tracks pitch so the brightness is the same across
// the keyboard.
//
//   freq   Hz, 130..1760   gain  0..1    glide  seconds
//   gate   held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 440, 130, 1760, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
glide = hslider("glide", 0.04, 0, 0.5, 0.001);
gate  = button("gate");

f = kl.lag(glide, freq) * (1.0 + 0.007 * kl.sine(5.6) * kl.lag(0.35, gate));
env = kl.adsr(0.008, 0.18, 0.75, 0.15, gate);
osc = kl.saw(f) * 0.6 + kl.pulse(f * kl.cents(7.0), 0.3) * 0.4;

process = osc : kl.lp(f * 5.0, 1.4) : *(env * gain * 0.9) : kl.soft;
