// SPDX-License-Identifier: GPL-3.0-only
//
// brass — a swelling sawtooth: the synth horn section.
//
// What makes a trumpet or a horn "brassy" is physical: the louder the player
// blows, the more the lip buzz is driven into a hard-edged waveform, so
// loudness and brightness rise TOGETHER. That correlation is the entire
// instrument, and it is why a plain saw through a fixed lowpass sounds like a
// synth and this does not. Here a second, slower envelope opens the filter
// while the amplitude envelope rises, with the cutoff following the SQUARE of
// it so the bright top arrives late in the swell. A trace of breath noise
// covers the start.
//
//   freq  Hz, 82..880     gain  0..1
//   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 233, 82, 880, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

amp = kl.adsr(0.045, 0.12, 0.85, 0.16, gate);
bri = kl.adsr(0.10, 0.2, 0.7, 0.2, gate);
breath = no.noise * kl.decay(0.06, kl.edge(gate)) : kl.lp1(2500.0) : *(0.05);
osc = (kl.saw(freq * kl.cents(3.0)) + kl.saw(freq * kl.cents(-3.0))) * 0.5;

process = osc : kl.lp(freq * (1.2 + 8.0 * bri * bri), 1.2) : +(breath) : *(amp * gain * 0.9);
