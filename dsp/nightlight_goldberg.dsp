// SPDX-License-Identifier: MIT
// After J.S. Bach, Goldberg Variations, Aria (PD). Sine choir in G, not the
// aria. Bake only.

import("stdfaust.lib");
gain = hslider("gain", 0.22, 0, 1, 0.01);
osc3(f) = os.osc(f) + os.osc(f*1.997)*0.16 + os.osc(f*3.003)*0.05;
pad = osc3(49.00)*0.42 + osc3(73.42)*0.34 + osc3(98.00)*0.24 + osc3(147.0)*0.12
    + osc3(196.0)*0.06
    : fi.lowpass(3, 1600)
    : re.mono_freeverb(0.42, 0.55, 0.35, 0.18);
soft(x) = k * (1.5 - 0.5 * k * k) with { k = max(-1.0, min(1.0, x)); };
process = pad * gain : soft;
