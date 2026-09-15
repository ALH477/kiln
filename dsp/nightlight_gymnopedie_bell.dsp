// SPDX-License-Identifier: MIT
// Sparse D bells over Gymnopédie. 5s grid, 20s loop.

import("stdfaust.lib");
gain = hslider("gain", 0.14, 0, 1, 0.01);
hit = os.lf_imptrain(0.2);
bell(f) = os.osc(f)*en.ar(0.03, 6.0, hit) + os.osc(f*1.50)*0.22*en.ar(0.02, 3.5, hit);
layer = bell(146.83)*0.48 + bell(220.00)*0.30 + bell(293.66)*0.20
      : fi.lowpass(2, 2000);
soft(x) = k * (1.5 - 0.5 * k * k) with { k = max(-1.0, min(1.0, x)); };
process = layer * gain : soft;
