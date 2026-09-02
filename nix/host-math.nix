# SPDX-License-Identifier: MIT
#
# nix/host-math.nix — libdragon's OWN fast-math library, compiled for the host.
#
# The engine leans on `fm_vec3_t` in 392 places and on 17 `fm_*` functions, and
# every one of them has to mean the same thing natively as it does on the
# VR4300. nix/checks/stub/t3d/t3dmath.h used to get there by copying the
# definitions in, and its own header explains at length why a copy is the only
# honest way to fake it:
#
#   "a stub that computes something subtly different turns every downstream
#    assertion into a test of the stub" ... "copy the real definition in — do
#    NOT approximate it. A single-precision reciprocal or a fast-inverse-sqrt
#    that differs from libdragon's in the last bit will make a host check
#    disagree with the console for reasons that have nothing to do with the
#    code being tested."
#
# This derivation retires that argument by not copying anything. libdragon's
# include/fgeom.h is 661 lines of plain C with no MIPS anything in it, and
# src/math/{fmath,fgeom}.c compile natively as they stand. So the host gets the
# REAL implementation — the same polynomial approximations, the same union
# layout, the same last bit — rather than a good-faith reproduction of it.
#
# ── The one patch, and why it changes no results ───────────────────────
# include/fmath.h has exactly four functions that are not portable:
# fm_truncf, fm_ceilf, fm_floorf and fm_roundf, each two lines of MIPS inline
# asm. libdragon documents every one of them as an "Optimized version using
# the MIPS <insn> instruction" of the corresponding libm call — they are a
# speed choice, not a semantics choice, so substituting the libm call back in
# is exactly value-preserving. nix/checks/kiln-hostmath.nix asserts that over
# a sweep rather than taking this paragraph's word for it.
#
# round.w.s is the one that needs care: MIPS rounds ties to EVEN, while C's
# roundf rounds ties AWAY FROM ZERO. nearbyintf under the default rounding
# mode is the tie-to-even one, so that is what it becomes.
#
# --replace-fail, not sed: a libdragon bump that reshapes these four functions
# must fail the build loudly, the same discipline nix/patches/tiny3d-load-buf.patch
# is held to. (A silent miss would in fact still fail, because the surviving
# asm cannot assemble for x86 — but "fails for the right reason" is worth the
# one flag.)
#
# ── One derivation, any toolchain ──────────────────────────────────────
# cc/ar default to stdenv's, which is what a native build wants. A cross or
# Emscripten target passes its own, plus whatever nativeBuildInputs and
# preBuild that compiler needs. Nothing else here is architecture-aware: the
# patch names MIPS mnemonics and substitutes ISO C, and the sources are plain
# portable C. See nix/host.nix, which is the only caller that passes them.
{ pkgs, src, pname ? "kiln-host-math", cc ? "$CC", ar ? "$AR"
, nativeBuildInputs ? [], preBuild ? "" }:

pkgs.stdenv.mkDerivation {
  inherit pname nativeBuildInputs;
  version = "unstable-${builtins.substring 0 7 (src.rev or "dirty")}";
  inherit src;

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    ${preBuild}

    mkdir -p stage/include stage/math
    cp include/fmath.h include/fgeom.h include/fgeom2d.h stage/include/
    # debug.h and utils.h are needed to COMPILE fmath.c and are deliberately
    # not installed: a consumer of this prefix wants the math, and at P2 the
    # host <libdragon.h> is a hand-written file that must not collide with
    # libdragon's own internal headers.
    cp include/debug.h stage/
    cp src/utils.h stage/
    cp src/math/fmath.c src/math/fgeom.c stage/math/

    substituteInPlace stage/include/fmath.h \
      --replace-fail '__asm ("trunc.w.s  %0,%1" : "=f"(yint) : "f"(x));' \
                     'yint = truncf(x);' \
      --replace-fail '__asm ("ceil.w.s  %0,%1" : "=f"(yint) : "f"(x));' \
                     'yint = ceilf(x);' \
      --replace-fail '__asm ("floor.w.s  %0,%1" : "=f"(yint) : "f"(x));' \
                     'yint = floorf(x);' \
      --replace-fail '__asm ("round.w.s  %0,%1" : "=f"(yint) : "f"(x));' \
                     'yint = nearbyintf(x);'
    # Four call sites of cvt.s.w, one per function above. Everything stays in
    # a float the whole way, so the int-to-float convert is an assignment.
    substituteInPlace stage/include/fmath.h \
      --replace-fail '__asm ("cvt.s.w  %0,%1" : "=f"(y) : "f"(yint));' \
                     'y = yint;'

    if grep -q '__asm' stage/include/fmath.h; then
      echo "ERROR: MIPS asm survives in the host fmath.h:" >&2
      grep -n '__asm' stage/include/fmath.h >&2
      exit 1
    fi

    # -include assert.h: libdragon's debug.h uses assert() without including
    # it, which is fine in its own tree and not here.
    for f in stage/math/fmath.c stage/math/fgeom.c; do
      # -ffp-contract=off for the reason nix/host.nix sets it on everything
      # else, and this file needs it stated because it is the one compilation
      # unit in the host tier that nix/host.nix's flags do NOT reach: it is
      # built here, at -O2, which is exactly the optimisation level where that
      # file's own measurement table shows GCC emitting 74 fused
      # multiply-adds on aarch64. kiln_engine.c calls fm_mat4_from_axis_angle,
      # fm_sinf and fm_cosf out of this archive on the path every byte-for-byte
      # render gate exercises, so leaving it out would put the one library the
      # guard does not cover underneath the claim the guard exists to protect.
      ${cc} -c -O2 -std=gnu11 -ffp-contract=off -include assert.h \
          -Istage/include -Istage -o "$f.o" "$f"
    done
    ${ar} rcs libkilnmath.a stage/math/fmath.c.o stage/math/fgeom.c.o

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out/include $out/lib
    install -m 0644 stage/include/fmath.h   $out/include/
    install -m 0644 stage/include/fgeom.h   $out/include/
    install -m 0644 stage/include/fgeom2d.h $out/include/
    install -m 0644 libkilnmath.a           $out/lib/
    runHook postInstall
  '';

  meta.description = "libdragon's fm_* fast-math library, built natively for the host";
}
