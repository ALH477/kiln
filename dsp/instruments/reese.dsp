// SPDX-License-Identifier: MIT
//
// reese — detuned saws beating through a lowpass: the drum-and-bass growl.
//
// Named for Kevin Saunderson's "Reese" (1988) and the foundation of a whole
// genre's low end. Three sawtooths a few cents apart do not sit still: the
// detuned pair drifts in and out of phase at a few hertz, a slow cancellation
// and reinforcement that the ear takes as a moving, snarling edge on a note
// that is otherwise steady. A sine underneath keeps the fundamental solid while
// that happens above it, and the lowpass (tracking the note, with a bit of
// resonance) keeps the top from becoming fizz.
//
//   freq    Hz, 30..130   gain  0..1
//   detune  cents spread (the beat rate follows: ~0.3 Hz per cent at 55 Hz)
//   bright  filter cutoff as a multiple of freq
//   gate    held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq   = hslider("freq", 55, 30, 130, 0.01);
gain   = hslider("gain", 0.5, 0, 1, 0.01);
detune = hslider("detune", 14, 1, 40, 0.1);
bright = hslider("bright", 5, 1, 12, 0.1);
gate   = button("gate");

env = kl.adsr(0.01, 0.2, 0.9, 0.18, gate);
saws = (kl.saw(freq * kl.cents(-detune)) + kl.saw(freq) + kl.saw(freq * kl.cents(detune))) * 0.33;

process = (saws : kl.lp(freq * bright, 1.6)) + 0.5 * kl.sine(freq) : *(env * gain * 0.6) : kl.soft;
