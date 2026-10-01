// SPDX-License-Identifier: MIT
//
// bass_808 — the long, tuned, saturated kick: the 808 bass.
//
// The Roland TR-808's bass drum is a sine whose pitch starts a little high and
// settles in a few tens of milliseconds, with an amplitude that rings for a
// second or more, and the genre trick is to play it as a bass NOTE: tune the
// sine to the key and let it sustain. Its weight comes from the settle (the
// "boom" at the front) and its audibility on small speakers from a drive stage:
// a soft saturator after the oscillator makes odd harmonics, so the note
// carries on a phone that cannot reproduce 40 Hz. `drive` is how much.
//
//   freq   Hz, 30..110     gain  0..1
//   decay  seconds to -60 dB     drive  0..1
//   gate   rising edge hits (it rings out; release does not cut it)

import("stdfaust.lib");
kl = library("kiln.lib");

freq  = hslider("freq", 49, 30, 110, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 1.1, 0.2, 3, 0.01);
drive = hslider("drive", 0.5, 0, 1, 0.01);
gate  = button("gate");

trg = kl.edge(gate);
f = freq * (1.0 + 0.8 * kl.decay(0.025, trg));
body = kl.psin(kl.phasor_r(f, trg)) * kl.decay(decay, trg);
click = no.noise * kl.decay(0.004, trg) : kl.lp1(1800.0);

process = (body * (1.0 + 3.0 * drive) + click * 0.15) : kl.tnh : *(gain * 1.1);
