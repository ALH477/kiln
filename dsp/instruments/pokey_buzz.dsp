// SPDX-License-Identifier: MIT
//
// pokey_buzz — an Atari POKEY "distortion": a pulse gated by a fixed bit pattern.
//
// POKEY's distortion modes AND the square wave with the output of a polynomial
// counter, a fixed pseudo-random bit pattern, so the tone comes out buzzy and
// gritty and still pitched: that is the sound of Atari 8-bit games and of
// Missile Command's and Asteroids' era. Here a 17-step pattern is a literal
// table read once per cycle, so the output is exactly periodic at `freq`. A
// real polynomial counter is longer; the character is the same.
//
//   freq  Hz, 65..1046   gain  0..1   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 220, 65, 1046, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

bit = rdtable(waveform{1,1,0,1,0,0,1,1,1,0,0,1,0,1,0,0,1}, int(kl.phasor(freq) * 17.0));
env = kl.adsr(0.002, 0.1, 0.8, 0.04, gate) : kl.quant(15.0);

process = (bit * 2.0 - 1.0) * env * (gain * 1.07);
