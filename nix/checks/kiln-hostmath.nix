# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-hostmath.nix — the host math is the console math.
#
# nix/host-math.nix substitutes four libm calls for four MIPS instructions on
# the claim that libdragon documents them as optimisations of exactly those
# calls. That claim is load-bearing for everything the engine computes, so it
# gets asserted rather than believed. See the check's C for what and why.
#
# Verified to fire in both directions: swapping nearbyintf for roundf in
# host-math.nix (the tie-breaking rule) fails this on the halfway inputs.
{ pkgs, hostMath }:

pkgs.runCommand "check-kiln-hostmath"
{
  nativeBuildInputs = [ pkgs.gcc ];
  meta.description = "libdragon's fm_* means the same thing on the host as on the VR4300";
}
  ''
    set -euo pipefail
    gcc -O1 -std=gnu11 -Wall -Wextra -Werror \
        -I${hostMath}/include -o hostmath_check \
        ${./kiln-hostmath-check.c} ${hostMath}/lib/libkilnmath.a -lm
    ./hostmath_check
    mkdir -p $out && touch $out/ok
  ''
