# SPDX-License-Identifier: MIT
#
# nix/checks/exsec-dungeon-parity.nix -- the committed Exsecutor dungeon
# generator still makes the chunks its oracle says it does.
#
# examples/exsec-dungeon-demo/dungeon_x86_64.gen.c is Exsecutor output committed
# into this repo, and so is dungeon_mips64.gen.c, which the ROM links. Neither
# is readable in review (85 KB of emitted C each), so this is what stands in for
# reading them: dungeon_parity.c re-runs the Exsecutor repository's own
# certificate -- tests/programs/dungeon/ -- on the committed unit, comparing a
# stream byte for byte with dungeon_expected.bin, which an independent Python
# implementation wrote. See that file's header for the four things it checks.
#
# ── Why this needs no `exsc` ───────────────────────────────────────────
# The same reason fig-pose-parity.nix gives: Kiln does not build the Exsecutor
# compiler. PROVENANCE.md pins the compiler's commit and the units' sha256, and
# this check is the other half -- that the bytes actually in the tree still
# compute what the oracle computes.
#
# ── Why the x86_64 unit stands in for the mips64 one ───────────────────
# The two emissions export the same functions and differ in the pointer-width
# assert and, in their bodies, in the 32-bit `mensura` (PROVENANCE.md measures
# how). This check holds the interface equal and nothing more, so "the host
# unit passed" is NOT evidence about the console unit's behaviour: the mips64 unit's behaviour is measured by Exsecutor's cross phase
# (big-endian, under qemu, n32 as the proxy ABI), not here, and on the console
# by the ROM's own boot-time check, which has not been run.
#
# ── Why four builds ────────────────────────────────────────────────────
# Exsecutor's differential rig holds its C backend to byte-identical output
# from gcc and clang at -O0 and -O2 under UBSan; a consumer checking under one
# compiler would be held to a weaker standard than the producer. clang runs with
# -fsanitize-trap=undefined where the host's clang ships no UBSan runtime; a
# trap is still an abort, so UB still fails the build.
{ pkgs, demoSrc }:

pkgs.runCommand "check-exsec-dungeon-parity"
{
  nativeBuildInputs = [ pkgs.gcc pkgs.clang pkgs.diffutils ];
  meta.description = "the committed Exsecutor dungeon unit matches its oracle";
}
  ''
    set -euo pipefail
    cp ${demoSrc}/*.c ${demoSrc}/*.h ${demoSrc}/dungeon_expected.bin .
    chmod -R u+w .

    # The two committed units are one program emitted for two rows. What must
    # match is the interface: the same functions with the same prototypes, so
    # exsec_dungeon.h is right for both. What must differ is the pointer-width
    # assert. (Their bodies differ in width constants, 32-bit normalisations
    # and array-slot sizes -- PROVENANCE.md measures that -- and no pattern
    # over those lines would be anything but a restatement of the compiler.)
    protos() { grep -E '^uint64_t exs_[a-z0-9_]+\(.*\);$' "$1" | sort; }
    protos dungeon_x86_64.gen.c > p_x86.txt
    protos dungeon_mips64.gen.c > p_mips.txt
    [ -s p_x86.txt ] || { echo "FAILED: no prototypes found in the x86_64 unit"; exit 1; }
    if ! cmp -s p_x86.txt p_mips.txt; then
      echo "FAILED: the two rows export different functions:"
      diff p_x86.txt p_mips.txt || true
      exit 1
    fi
    echo "  both rows export the same $(wc -l < p_x86.txt) functions"
    grep -q '_Static_assert(sizeof(void \*) == 8,' dungeon_x86_64.gen.c
    grep -q '_Static_assert(sizeof(void \*) == 4,' dungeon_mips64.gen.c
    # The unit refuses -ffast-math in its prologue; the Makefile must not pass it.
    grep -q 'exsecutor: -ffast-math is refused' dungeon_mips64.gen.c

    fail=0
    for cc in gcc clang; do
      for opt in -O0 -O2; do
        tag="$cc$opt"
        san="-fsanitize=undefined -fno-sanitize-recover=all"
        [ "$cc" = clang ] && san="-fsanitize=undefined -fsanitize-trap=undefined"
        if ! $cc -std=c11 $opt $san -Wall -Wextra -Wno-unused-function \
             -o "p_$tag" dungeon_parity.c dungeon_view.c dungeon_x86_64.gen.c \
             2>"cc_$tag.log"; then
          echo "FAILED to build with $cc $opt:"; sed 's/^/    /' "cc_$tag.log"
          fail=1; continue
        fi
        if ! "./p_$tag" dungeon_expected.bin > "out_$tag.txt" 2>&1; then
          echo "FAILED at run time with $cc $opt:"; sed 's/^/    /' "out_$tag.txt"
          fail=1; continue
        fi
        printf '  %-10s %s\n' "$tag" "$(tail -1 "out_$tag.txt")"
      done
    done
    [ "$fail" = 0 ] || exit 1

    n=$(md5sum out_*.txt | awk '{print $1}' | sort -u | wc -l)
    if [ "$n" != 1 ]; then
      echo "FAILED: the four builds did not agree with each other:"
      md5sum out_*.txt | sed 's/^/    /'
      exit 1
    fi
    echo "the committed dungeon unit reproduces the oracle's stream, identically under all four builds"
    touch $out
  ''
