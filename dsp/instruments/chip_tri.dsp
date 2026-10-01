// SPDX-License-Identifier: GPL-3.0-only
//
// chip_tri — the NES triangle: a 4-bit staircase triangle, on or off.
//
// The triangle channel has no volume control at all — it is on or it is off —
// and its 32-step, 4-bit waveform is the buzzy, hollow bass under every
// Famicom soundtrack. Quantising the triangle to sixteen levels adds the odd
// harmonics that make it sound like that rather than like a textbook triangle.
// The only envelope is a 2 ms click-suppressor on the gate.
//
//   freq  Hz, 33..880   gain  0..1   gate  held = sounds

import("stdfaust.lib");
kl = library("kiln.lib");

freq = hslider("freq", 110, 33, 880, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

process = kl.tri4(freq) * kl.lag(0.003, gate) * (gain * 1.15);
