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

import("stdfaust.lib");

freq = hslider("freq", 220, 50, 2000, 0.01);
gain = hslider("gain", 0.5, 0, 1, 0.01);
gate = button("gate");

process = pm.ks(freq, 0.5, gate) * gain <: _, _;
