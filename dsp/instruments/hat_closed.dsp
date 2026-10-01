// SPDX-License-Identifier: MIT
//
// hat_closed — a closed hi-hat: six metal oscillators, filtered, and shut off.
//
// The classic electronic hat is not noise. It is six square waves at
// deliberately unrelated frequencies (the ratios below are the ones a 1970s
// drum machine's metallic circuit settled on) summed and highpassed: their
// many mutual sum-and-difference tones fill the top of the spectrum with dense,
// inharmonic, metallic energy that noise only approximates. The squares are
// band-limited here; naive ones would fold that energy back into the audible
// range as a hiss that sits on top of everything.
//
//   tune   0.5..2, a multiplier on all six oscillators
//   gain   0..1    decay  seconds    gate  rising edge hits

import("stdfaust.lib");
kl = library("kiln.lib");

tune  = hslider("tune", 1, 0.5, 2, 0.01);
gain  = hslider("gain", 0.5, 0, 1, 0.01);
decay = hslider("decay", 0.055, 0.015, 0.2, 0.001);
gate  = button("gate");

metal(t) = (kl.square(205.3 * t) + kl.square(304.4 * t) + kl.square(369.6 * t)
          + kl.square(522.7 * t) + kl.square(540.0 * t) + kl.square(800.0 * t)) * 0.16;

trg = kl.edge(gate);
sizzle = no.noise : kl.hp(6000.0, 0.7) : *(0.25);

process = (metal(tune) : kl.hp(7000.0, 1.0)) + sizzle : *(kl.decay(decay, trg) * 1.3) : kl.soft : *(gain * 1.0);
