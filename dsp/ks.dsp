// SPDX-License-Identifier: MIT
//
// A Karplus-Strong plucked string — the worked example for the live
// on-console path, and the regression test for nix/faust.nix's gates.
//
// Why this one: report §4 puts the realistic VR4300 budget at "1-3 simultaneous
// waveguide voices", and identifies waveguides/long delay lines as the
// structures that survive the drop from double to single precision intact
// (unlike high-Q biquads and Thiran allpasses, which are the danger zone).
// So a plucked string is both representative and the most likely thing to
// actually ship as a live voice.
//
// It deliberately uses no tanh/exp/pow: those would map to libm and the
// no-libm gate would reject the build. If you add saturation here, tabulate it
// with ba.tabulate rather than calling the transcendental.
//
// ── `freq` is a frequency, and pm.ks wants a LENGTH ────────────────────
// This file used to pass `freq` straight into pm.ks, whose first argument is
// the string's length in METRES, clamped to pm.maxLength (3 m). 220 "metres"
// clamped to the longest string there is, so every value of the slider
// rendered byte-identical output — the baked asset at 220, and every one of
// examples/live-voice's seven "frequencies". pm.f2l converts; the slider's
// floor is 120 Hz because a lower note needs a string longer than 3 m.
//
// ── and the excitation was a DC step ───────────────────────────────────
// `gate` itself was the excitation: a 20 ms rectangle, which a waveguide turns
// into a decaying DC offset (mean 11350 of a 23658 peak, no measurable pitch).
// A string is plucked with a burst of noise one period long, and fi.dcblocker
// takes out what DC the loop still accumulates. no.noise is an integer LCG
// and dcblocker is one pole and one zero, so neither goes near libm.

import("stdfaust.lib");

freq = hslider("freq", 220, 120, 2000, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

// One period of noise from the rising edge of the gate.
trig = gate > gate';
burst = no.noise * (ba.countdown(ma.SR / freq, trig) > 0);

process = pm.ks(pm.f2l(freq), 0.02, burst) : fi.dcblocker : *(gain) <: _, _;
