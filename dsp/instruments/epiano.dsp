// SPDX-License-Identifier: GPL-3.0-only
//
// epiano — a tine electric piano, by two-operator frequency modulation.
//
// The classic FM electric piano is two pairs. The BODY is a sine modulated by a
// sine at the same frequency, with a modulation index that starts high and
// falls quickly: a hard, barking attack that mellows into a bell-like sustain
// as the index dies — exactly what a struck tine does as its upper partials
// ring out faster than its fundamental. The TINE is a second pair with the
// modulator at fourteen times the carrier: a brief inharmonic "ping" under
// the first 80 ms and nothing after. Key velocity (`gain`) scales the index too,
// so a hard touch is brighter as well as louder — which a real piano does and a
// sampled one with a volume knob does not.
//
//   freq   Hz, 65..1046   gain  0..1    decay  seconds of body sustain
//   gate   rising edge starts a note; releasing damps it

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 262, 65, 1046, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 1.6, 0.2, 6, 0.01);
gate  = button("gate");

amp = kl.adsr(0.002, decay, 0.0, 0.18, gate);
idx = kl.adsr(0.001, decay * 0.25, 0.0, 0.1, gate);
ping = kl.decay(0.07, kl.edge(gate));

// Modulation index in turns: 1 radian = 0.159 turns.
body = kl.sine_pm(freq, 0.16 * (0.4 + 2.2 * gain) * idx * kl.sine(freq));
tine = kl.sine_pm(freq, 0.12 * ping * kl.sine(freq * 14.0)) * 0.25;
trem = 1.0 - 0.07 * (0.5 + 0.5 * kl.sine(4.4));

process = (body * amp + tine * amp) * trem * (gain * 0.7) : kl.soft;
