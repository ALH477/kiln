// SPDX-License-Identifier: MIT
//
// chip_pulse — a NES-style pulse channel: naive square, stepped volume, late vibrato.
//
// The Famicom's two pulse channels are the sound of a generation: a square wave
// with four duty cycles (12.5%, 25%, 50%, 75%), a 4-bit volume register, and
// nothing else (the DC a narrow duty leaves is blocked, as the console's output
// capacitor blocked it). Nothing band-limits it, so it aliases, and that grit is the
// timbre; `kl.npulse` is one compare per sample for that reason. Volume is
// quantised to sixteen steps because a 4-bit register cannot do smooth fades,
// and the decay steps audibly. Vibrato is a triangle LFO fading in after a
// beat, which is how sequencers drove the pitch register.
//
//   freq  Hz, 65..2093   gain  0..1   duty  0.125..0.5 (0.5 = hollow, 0.125 = thin)
//   vib   0..1           gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 440, 65, 2093, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
duty = hslider("duty", 0.25, 0.125, 0.5, 0.125);
vib  = hslider("vib", 0.3, 0, 1, 0.01);
gate = button("gate");

env = kl.adsr(0.002, 0.18, 0.7, 0.05, gate) : kl.quant(15.0);
f = freq * (1.0 + 0.006 * vib * kl.tri(6.0) * kl.lag(0.25, gate));

process = kl.npulse(f, duty) : kl.dcblock : *(env * gain * 1.07);
