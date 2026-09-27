# SPDX-License-Identifier: MIT
#
# nix/checks/fig-pose-parity.nix — the emitted core agrees with the C it sits
# beside.
#
# engine/src/kiln/gen/fig_pose_*.gen.c is Exsecutor output committed into this
# repo: written once as fig_pose.exsc, emitted per --hospes row, and meant to
# produce the same bits on the VR4300 as on x86-64. That is a claim, and a
# committed generated file is exactly the kind of claim that rots quietly —
# nobody reads 28,703 bytes of emitted C in review.
#
# ── Why this check needs no `exsc` ─────────────────────────────────────
# It compiles the COMMITTED emission, not the .exsc. Kiln does not build the
# Exsecutor compiler and should not start: gen/PROVENANCE.md pins the compiler
# commit, its byte count and its sha256, and that is the reproducibility
# story. What this check adds is the other half — that the bytes actually in
# the tree still compute what kiln_pose.c computes. A regeneration that
# changed behaviour fails here even though the provenance table still parses.
#
# ── Why zero tolerance ─────────────────────────────────────────────────
# fig_quat_mul is four multiplies and three add/subs per lane. No division, no
# root, and nothing a compiler may reassociate on IEEE floats. So memcmp is
# the right operator and any tolerance would hide the divergence this exists
# to catch: -ffast-math on one side, a contracted multiply-add on the other,
# or an emitted expression that reassociated.
#
# ── Why four builds ────────────────────────────────────────────────────
# Exsecutor's own differential rig requires byte-identical output from gcc and
# clang at -O0 and -O2 under UBSan with -fno-sanitize-recover=all, and holds
# its C backend to that. A core emitted by that compiler and then checked
# under one compiler at one optimisation level would be held to a weaker
# standard downstream than upstream — which is the wrong direction for a
# consumer to relax.
{ pkgs, engineSrc }:

pkgs.runCommand "check-fig-pose-parity"
{
  nativeBuildInputs = [ pkgs.gcc pkgs.clang ];
  meta.description = "the emitted fig_pose core matches kiln_pose.c";
}
  ''
    set -euo pipefail
    cp ${engineSrc}/src/kiln/exsc/fig_pose_parity.c parity.c
    cp ${engineSrc}/src/kiln/gen/fig_pose_x86_64.gen.c core.c

    # The mips64 row is not run here: it needs a cross toolchain and qemu, and
    # this repo has neither. gen/PROVENANCE.md records that its verification
    # stops at "compiles for the console row", so nothing here implies more.

    fail=0
    for cc in gcc clang; do
      for opt in -O0 -O2; do
        tag="$cc$opt"
        if ! $cc -std=c11 $opt -fsanitize=undefined -fno-sanitize-recover=all \
             -o "p_$tag" parity.c core.c -lm 2>"cc_$tag.log"; then
          echo "FAILED to build with $cc $opt:"; sed 's/^/    /' "cc_$tag.log"
          fail=1; continue
        fi
        if ! "./p_$tag" > "out_$tag.txt" 2>&1; then
          echo "FAILED at run time with $cc $opt:"; sed 's/^/    /' "out_$tag.txt"
          fail=1; continue
        fi
        printf '  %-10s %s\n' "$tag" "$(tail -1 "out_$tag.txt")"
      done
    done
    [ "$fail" = 0 ] || exit 1

    # Byte-identity across the four, not merely four passes. Four builds that
    # each agree with kiln_pose.c by a DIFFERENT route would still be a
    # finding — it would mean something in the emission is
    # optimisation-dependent.
    n=$(md5sum out_*.txt | awk '{print $1}' | sort -u | wc -l)
    if [ "$n" != 1 ]; then
      echo "FAILED: the four builds did not agree with each other:"
      md5sum out_*.txt | sed 's/^/    /'
      exit 1
    fi
    echo "the emitted core matches kiln_pose.c, identically under all four builds"
    touch $out
  ''
