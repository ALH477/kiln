// SPDX-License-Identifier: MIT
// Sparse G bells over the Goldberg pad. Hits every 5s so a 20s loop joins.

import("stdfaust.lib");
gain = hslider("gain", 0.16, 0, 1, 0.01);
hit = os.lf_imptrain(0.2);
bell(f) = os.osc(f)*en.ar(0.02, 4.5, hit) + os.osc(f*2.01)*0.28*en.ar(0.01, 2.8, hit);
layer = bell(196.00)*0.55 + bell(293.66)*0.28 + bell(392.00)*0.18
      : fi.lowpass(2, 2400);
soft(x) = k * (1.5 - 0.5 * k * k) with { k = max(-1.0, min(1.0, x)); };
process = layer * gain : soft;
