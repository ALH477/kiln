// SPDX-License-Identifier: MIT
// After Satie, Gymnopédie No.1 (PD). Sparse D-major sine colour. Bake only.

import("stdfaust.lib");
gain = hslider("gain", 0.20, 0, 1, 0.01);
osc3(f) = os.osc(f) + os.osc(f*2.003)*0.12 + os.osc(f*2.997)*0.04;
pad = osc3(73.42)*0.38 + osc3(98.00)*0.30 + osc3(110.00)*0.22 + osc3(146.83)*0.12
    : fi.lowpass(3, 1300)
    : re.mono_freeverb(0.55, 0.62, 0.40, 0.22);
soft(x) = k * (1.5 - 0.5 * k * k) with { k = max(-1.0, min(1.0, x)); };
process = pad * gain : soft;
