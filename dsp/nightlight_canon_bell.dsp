// SPDX-License-Identifier: MIT
// Sparse A bells over the Canon ground. 5s grid, 20s loop.

import("stdfaust.lib");
gain = hslider("gain", 0.15, 0, 1, 0.01);
hit = os.lf_imptrain(0.2);
bell(f) = os.osc(f)*en.ar(0.015, 4.0, hit) + os.osc(f*2.02)*0.30*en.ar(0.01, 2.4, hit);
layer = bell(220.00)*0.50 + bell(329.63)*0.28 + bell(440.00)*0.18
      : fi.lowpass(2, 2600);
soft(x) = k * (1.5 - 0.5 * k * k) with { k = max(-1.0, min(1.0, x)); };
process = layer * gain : soft;
