# SPDX-License-Identifier: MIT
#
# nix/checks/toolchain.nix — the toolchain regression test (report §2, §4).
#
# This started as the M0 go/no-go spike and is kept as a check because every
# assumption it makes is one that a nixpkgs bump could silently break. It
# verifies, against the real compiler:
#
#   * the mips64-elf-* prefix aliases resolve and report a big-endian target;
#   * libdragon's exact codegen flags are accepted (-march/-mtune=vr4300 with
#     -mabi=o64 — o64 is the ABI n64.mk uses, and it needs a matching newlib
#     multilib, which is the single most likely thing to break);
#   * newlib's libc AND libm actually LINK for that ABI, not merely compile —
#     libdragon links -lm, so a missing o64 libm multilib must fail here rather
#     than at ROM-link time;
#   * single-precision float codegen emits no double-precision instructions.
#     Report §2: DIV.D is 58 cycles against a ~2126-cycle per-sample budget, so
#     an accidental promotion to double is a correctness-of-performance bug.
{ pkgs, toolchain }:

pkgs.runCommand "check-toolchain"
{
  nativeBuildInputs = [ toolchain ];
  meta.description = "vr4300 codegen and newlib o64 multilib regression test";
}
  ''
    set -euo pipefail
    export PATH="${toolchain}/bin:$PATH"
    CC=mips64-elf-gcc

    echo "── toolchain identity ──"
    $CC --version | head -1
    $CC -dumpmachine

    # Big-endian is not negotiable: the N64 is MIPS III big-endian, and a
    # little-endian toolchain would produce a ROM that boots to nothing.
    if ! $CC -dumpmachine | grep -q '^mips64-'; then
      echo "FAIL: not a mips64 target" >&2; exit 1
    fi
    if $CC -dumpmachine | grep -q 'el-'; then
      echo "FAIL: little-endian toolchain" >&2; exit 1
    fi

    # libdragon's flags, verbatim from n64.mk's N64_C_AND_CXX_FLAGS.
    ARCH="-march=vr4300 -mtune=vr4300 -mabi=o64"

    echo "── compile with libdragon's flags ──"
    cat > t.c <<'EOF'
    float biquad(float x, float *s, const float *c) {
        float y = c[0]*x + s[0];
        s[0] = c[1]*x - c[3]*y + s[1];
        s[1] = c[2]*x - c[4]*y;
        return y;
    }
    int main(void) { return 0; }
EOF
    $CC $ARCH -O2 -c t.c -o t.o
    echo "  compile OK"

    # The real multilib test: linking pulls in newlib's libc.a and libm.a for
    # this ABI. If the o64 multilib is missing, this is where it shows up.
    echo "── link against newlib libc + libm ──"
    $CC $ARCH -O2 t.c -lm -o t.elf
    echo "  link OK"
    mips64-elf-size t.elf

    echo "── confirm vr4300 / big-endian in the artifact ──"
    mips64-elf-objdump -f t.elf | tee objdump-f.txt
    # objdump reports the BFD target, e.g. "elf32-bigmips". Note elf32 is
    # correct here and not a mistake: o64 is a 32-bit-address ABI with 64-bit
    # registers, which is exactly what libdragon targets.
    grep -q 'elf32-bigmips' objdump-f.txt \
      || { echo "FAIL: expected elf32-bigmips, got:" >&2; cat objdump-f.txt >&2; exit 1; }
    # "mips:4000" is the R4000 family the VR4300 belongs to.
    grep -q 'architecture: mips:4000' objdump-f.txt \
      || { echo "FAIL: not an R4000-family target:" >&2; cat objdump-f.txt >&2; exit 1; }

    # Single precision only. Any .d-suffixed FP op in a float-only translation
    # unit means something promoted to double behind our back.
    echo "── no double-precision instructions in float-only code ──"
    mips64-elf-objdump -d t.o > dis.txt
    if grep -qE '^\s+[0-9a-f]+:.*\b(add|sub|mul|div|sqrt|neg|abs|mov)\.d\b' dis.txt; then
      echo "FAIL: double-precision ops emitted from a float-only unit:" >&2
      grep -E '\.d\b' dis.txt >&2
      exit 1
    fi
    echo "  clean"

    echo
    echo "toolchain check PASSED"
    mkdir -p $out
    cp objdump-f.txt dis.txt $out/
  ''
