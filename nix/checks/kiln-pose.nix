# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-pose.nix — kiln_pose, asserted on the host.
#
# kiln_skel's overlay slot (an attack over a run) is a MASKED blend, and
# Tiny3D's t3d_skeleton_blend blends every bone, so that arithmetic is the
# engine's own. The host cannot run a skeleton at all — plat/host aborts on
# every t3d_skeleton_* call — but it can run this, over the console's own
# T3DBone layout (plat/host/include/t3d/t3dskeleton.h copies it).
#
# Against plat/host's real headers and libdragon's fast math, not
# nix/checks/stub: T3DBone needs <t3d/t3dskeleton.h>, and a stub layout would
# test the blend against a struct the console does not have.
{ pkgs, engineSrc, hostMath, platHost }:

pkgs.runCommand "check-kiln-pose"
{
  nativeBuildInputs = [ pkgs.gcc ];
  meta.description = "kiln_pose's masked bone blend, subtree masks and quaternion ops, on the host";
}
  ''
    set -euo pipefail
    SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"

    gcc -O1 -g -std=gnu2x -Wall -Wextra -Werror $SAN \
        -I${platHost}/include -I${hostMath}/include \
        -I${engineSrc}/src/kiln \
        -o kilnpose_check \
        ${./kiln-pose-check.c} \
        ${engineSrc}/src/kiln/kiln_pose.c \
        ${hostMath}/lib/libkilnmath.a -lm

    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
      ./kilnpose_check

    mkdir -p $out && touch $out/ok
  ''
