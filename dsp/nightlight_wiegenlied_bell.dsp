// SPDX-License-Identifier: MIT
// Sparse E-flat bells over Wiegenlied. 5s grid, 20s loop.

import("stdfaust.lib");
gain = hslider("gain", 0.16, 0, 1, 0.01);
hit = os.lf_imptrain(0.2);
bell(f) = os.osc(f)*en.ar(0.02, 5.0, hit) + os.osc(f*2.00)*0.25*en.ar(0.01, 3.0, hit);
layer = bell(155.56)*0.50 + bell(233.08)*0.32 + bell(311.13)*0.18
      : fi.lowpass(2, 2200);
soft(x) = k * (1.5 - 0.5 * k * k) with { k = max(-1.0, min(1.0, x)); };
process = layer * gain : soft;
