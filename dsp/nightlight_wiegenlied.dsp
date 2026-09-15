// SPDX-License-Identifier: MIT
// After Brahms, Wiegenlied Op.49 No.4 (PD). E-flat sine choir. Bake only.

import("stdfaust.lib");
gain = hslider("gain", 0.22, 0, 1, 0.01);
osc3(f) = os.osc(f) + os.osc(f*1.997)*0.16 + os.osc(f*3.003)*0.05;
pad = osc3(77.78)*0.40 + osc3(98.00)*0.32 + osc3(116.54)*0.26 + osc3(155.56)*0.14
    + osc3(196.0)*0.07
    : fi.lowpass(3, 1500)
    : re.mono_freeverb(0.48, 0.58, 0.38, 0.20);
soft(x) = k * (1.5 - 0.5 * k * k) with { k = max(-1.0, min(1.0, x)); };
process = pad * gain : soft;
