// SPDX-License-Identifier: GPL-3.0-only
//
// kick — a synthesised bass drum: a sine that falls in pitch, and a click.
//
// A kick drum head, struck, starts stretched tight and rings at a high pitch
// that drops quickly as the tension relaxes into the shell's resonance. That
// pitch fall IS the "thump": the body of the sound is a sine whose frequency
// starts at several times `freq` and decays exponentially back to it over
// about 40 ms. The phase restarts at zero on every hit so the click lines up
// the same way each time (a kick that starts at a random phase sounds different
// every beat). A burst of lowpassed noise is the beater; a soft clip adds the
// weight a clean sine lacks.
//
//   freq   Hz, 35..120 (the resting pitch)     gain   0..1
//   punch  0 = a soft thud   1 = a hard sweep from high above
//   decay  seconds to -60 dB     gate  rising edge hits

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 52, 35, 120, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
punch = hslider("punch", 0.6, 0, 1, 0.01);
decay = hslider("decay", 0.45, 0.1, 1.5, 0.001);
gate  = button("gate");

trg = kl.edge(gate);
f   = freq * (1.0 + 5.0 * punch * kl.decay(0.035, trg));
amp = kl.decay(decay, trg);
body = kl.psin(kl.phasor_r(f, trg)) * amp;
beater = no.noise * kl.decay(0.012, trg) : kl.lp1(2500.0);

process = (body * 1.6 + beater * 0.35) : kl.soft : *(gain * 1.0);
