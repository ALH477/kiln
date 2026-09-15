// SPDX-License-Identifier: MIT
// After Pachelbel, Canon in D (PD). Ground as a held D choir. Bake only.

import("stdfaust.lib");
gain = hslider("gain", 0.22, 0, 1, 0.01);
osc3(f) = os.osc(f) + os.osc(f*1.996)*0.15 + os.osc(f*3.002)*0.05;
pad = osc3(73.42)*0.36 + osc3(110.00)*0.30 + osc3(146.83)*0.24 + osc3(220.00)*0.12
    + osc3(293.66)*0.06
    : fi.lowpass(3, 1700)
    : re.mono_freeverb(0.40, 0.50, 0.32, 0.16);
soft(x) = k * (1.5 - 0.5 * k * k) with { k = max(-1.0, min(1.0, x)); };
process = pad * gain : soft;
