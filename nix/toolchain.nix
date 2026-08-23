# SPDX-License-Identifier: MIT
#
# nix/toolchain.nix — the mips64-elf cross toolchain for N64 targets.
#
# This is the one file that owns the toolchain strategy, and the only file that
# knows where the compiler comes from. Everything downstream depends on the
# `N64_GCCPREFIX` store path this produces, never on this expression directly,
# so the strategy can be swapped without touching consumers.
#
# Strategy: reuse nixpkgs' `pkgsCross.mips64-embedded` — a big-endian
# `mips64-none-elf` GCC + binutils + newlib. Four adaptations are needed:
#
#   1. Pin GCC 14 (see gccOverlay below).
#   2. Drop libgloss (see newlibOverlay below).
#   3. Rename the tool prefix: libdragon's `n64.mk` sets `N64_TARGET = mips64-elf`
#      and invokes `$(N64_GCCPREFIX)/bin/mips64-elf-gcc`, while nixpkgs produces
#      `mips64-none-elf-gcc`. We expose both names from one prefix.
#   4. Keep nixpkgs' cc-wrapper. It is what teaches gcc where nixpkgs' newlib
#      headers and libs live — an unwrapped `gcc.cc` cannot find them, because
#      nixpkgs derives newlib separately rather than installing it into gcc's
#      own prefix. Hardening is disabled per-derivation by consumers instead
#      (see rom.nix), since `-fPIE`/`-fstack-protector` are wrong for a ROM.
#
# If this ever fails on vr4300 codegen, replace this file with a derivation
# mirroring libdragon's `tools/build-toolchain.sh` (which configures GCC
# `--with-arch=vr4300 --enable-multilib` and newlib `--with-cpu=mips64vr4300`).
# Nothing else has to change.
{ nixpkgs, pkgs, system }:

let
  lib = pkgs.lib;

  # ── Why GCC 14 and not the nixpkgs default ────────────────────────────
  # Two independent reasons, pointing the same way:
  #
  #  * nixpkgs' current default (GCC 15.3.0) ICEs building libstdc++ for this
  #    target — "internal compiler error: in expand_fn_using_insn, at
  #    internal-fn.cc:268" on include/charconv. libdragon needs a working g++
  #    (it compiles src/debugcpp.cpp into libdragon.a), so we cannot simply
  #    drop the C++ frontend.
  #  * libdragon's own tools/build-toolchain.sh pins GCC_V=14.4.0, which is
  #    exactly nixpkgs' `gcc14`. Matching upstream's tested compiler is what
  #    we want regardless of the ICE.
  #
  # Revisit when nixpkgs' default GCC builds this target cleanly AND libdragon
  # has moved its own pin forward.
  gccOverlay = final: prev: { gcc = prev.gcc14; };

  # ── Why libgloss has to go ────────────────────────────────────────────
  # nixpkgs' newlib deliberately re-enables libgloss for cross targets by
  # patching it back out of the configure script's `noconfigdirs`/`cross_only`
  # lists. On mips64 that fails to assemble with binutils 2.46:
  #
  #   libgloss/mips/hal/crt0.S:92: Error: invalid operands `mtc0 $0,C0_CAUSE'
  #
  # libgloss is a board-support/syscall layer, and libdragon supplies its own:
  # `entrypoint.S`, `libdragonsys.a`, and `n64.ld` replace crt0 and the syscall
  # stubs entirely, and `n64.mk` links only `-ldragon -lm -ldragonsys`. So we
  # need newlib's libc and libm, and specifically do NOT want libgloss.
  #
  # Rather than pass `--disable-libgloss` (which the top-level configure does
  # not honour once nixpkgs has rewritten those lists), we narrow nixpkgs'
  # substitutions so that `target-newlib` is still built while `target-libgloss`
  # stays excluded. `--replace-fail` makes this break loudly if nixpkgs changes
  # the strings, rather than silently reintroducing the broken build.
  newlibOverlay = final: prev: {
    newlib = prev.newlib.overrideAttrs (old: {
      preConfigure = ''
        export CC=cc
        substituteInPlace configure \
          --replace-fail 'noconfigdirs target-newlib target-libgloss' \
                         'noconfigdirs target-libgloss'
        substituteInPlace configure \
          --replace-fail 'cross_only="target-libgloss target-newlib' \
                         'cross_only="target-libgloss'
      '';
    });
  };

  crossPkgs = import nixpkgs {
    inherit system;
    crossSystem = lib.systems.examples.mips64-embedded;
    overlays = [ gccOverlay newlibOverlay ];
  };

  # The nixpkgs tuple, and the tuple libdragon expects.
  nixTriple = crossPkgs.stdenv.cc.targetPrefix; # "mips64-none-elf-"
  n64Triple = "mips64-elf-";

  cc = crossPkgs.stdenv.cc; # the wrapper: gcc, g++, cc, cpp, + binutils re-exports
  bintools = crossPkgs.stdenv.cc.bintools; # wrapped as, ld, ar, nm, objdump, ...
  gccRaw = crossPkgs.stdenv.cc.cc; # unwrapped: also has gcc-ar/gcc-nm/gcc-ranlib

in
pkgs.runCommand "n64-toolchain-${crossPkgs.stdenv.cc.cc.version}"
{
  passthru = {
    inherit crossPkgs nixTriple n64Triple;
    inherit (crossPkgs.stdenv.cc.cc) version;
    newlib = crossPkgs.newlib;
  };

  meta = {
    description = "mips64-elf cross toolchain for Nintendo 64 targets";
    platforms = lib.platforms.linux;
  };
}
  ''
    mkdir -p $out/bin

    # Expose every cross tool under BOTH prefixes. The nixpkgs-native name is
    # kept so anything driving the compiler by its real tuple still works; the
    # mips64-elf-* aliases are what n64.mk looks for.
    #
    # Order matters and links never clobber: the WRAPPED gcc/g++/ld must win,
    # because the wrapper is what supplies newlib's header and library paths.
    # The unwrapped GCC is linked last purely to pick up the tools the wrapper
    # does not re-export — gcc-ar / gcc-nm / gcc-ranlib (the LTO-plugin-aware
    # archive tools; n64.mk's N64_AR is `gcc-ar`), plus gcov.
    link_all() {
      for f in "$1"/bin/${nixTriple}*; do
        [ -e "$f" ] || continue
        b="$(basename "$f")"
        stem="''${b#${nixTriple}}"
        [ -e "$out/bin/$b" ] || ln -s "$f" "$out/bin/$b"
        [ -e "$out/bin/${n64Triple}$stem" ] || ln -s "$f" "$out/bin/${n64Triple}$stem"
      done
    }

    link_all ${cc}
    link_all ${bintools}
    link_all ${gccRaw}

    # Sanity: the tools n64.mk dereferences by name. Failing here beats failing
    # deep inside a libdragon build with a confusing "command not found".
    for t in gcc g++ as ar ld objcopy objdump size nm strip gcc-ar; do
      if [ ! -e "$out/bin/${n64Triple}$t" ]; then
        echo "toolchain.nix: missing ${n64Triple}$t — the nixpkgs cross set changed shape" >&2
        exit 1
      fi
    done
  ''
