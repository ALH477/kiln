# SPDX-License-Identifier: MIT
#
# nix/engine.nix — the Kiln engine: 3D on Tiny3D, 2D GUI on rdpq.
#
# Packaged exactly like Tiny3D (same $N64_INST layout, own store path, merged
# by n64-inst.nix) so a ROM links it with a plain `-lkiln` and nothing here is
# special-cased.
#
# `n64InstBase` must be a prefix containing BOTH libdragon and Tiny3D: the
# engine's headers include <libdragon.h> and <t3d/t3d.h>. That is why the flake
# builds the prefix in two stages — libdragon+tiny3d first to compile against,
# then the engine merged in on top for ROMs to consume.
{ pkgs, src, toolchain, n64InstBase }:

pkgs.stdenv.mkDerivation {
  pname = "kiln-engine";
  version = "0.1.0";
  inherit src;

  nativeBuildInputs = [ toolchain n64InstBase pkgs.gnumake pkgs.which ];

  hardeningDisable = [ "all" ];

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    export N64_INST="${n64InstBase}"
    export N64_GCCPREFIX="${toolchain}"
    make -j"$NIX_BUILD_CORES"
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    export N64_INST="${n64InstBase}"
    export N64_GCCPREFIX="${toolchain}"
    mkdir -p $out/include $out/mips64-elf/lib $out/mips64-elf/include/kiln
    make install INSTALLDIR="$out"
    runHook postInstall
  '';

  dontStrip = true;
  dontPatchELF = true;

  doInstallCheck = true;
  # The header list is READ OUT OF THE MAKEFILE (`make print-modules`), not
  # restated here. It used to be restated, and it drifted: eleven installed
  # headers — console, context, dialogue, fpscam, inventory, panic, prof,
  # projectile, trigger, weapon, weapons — were never checked, because
  # adding a module meant editing four places and this was the place people
  # forgot. A check whose own list can fall behind the thing it checks is
  # not a check; asking make for its own variable removes the second list.
  installCheckPhase = ''
    runHook preInstallCheck
    export N64_INST="${n64InstBase}"
    export N64_GCCPREFIX="${toolchain}"

    for f in include/kiln.mk mips64-elf/lib/libkiln.a; do
      [ -e "$out/$f" ] || { echo "engine.nix: missing $f" >&2; exit 1; }
    done

    # Two lists: MODULES (each has a .c and contributes an object) and
    # print-headers (MODULES plus the header-only ones, which are installed but
    # compile into nothing — see engine/Makefile's HEADER_ONLY comment). The
    # header checks below use the second, or a header-only module would be
    # installed and unverified, which is the exact hole this check was built to
    # close.
    modules=$(make -s --no-print-directory print-modules)
    headers=$(make -s --no-print-directory print-headers)
    n=0
    for m in $headers; do
      f="mips64-elf/include/kiln/$m.h"
      [ -e "$out/$f" ] || { echo "engine.nix: missing $f" >&2; exit 1; }
      n=$((n + 1))
    done

    # A guard on the guard: an empty or mangled MODULES would make the loop
    # above pass vacuously, which is the same silent failure in a new shape.
    if [ "$n" -lt 40 ]; then
      echo "engine.nix: only $n modules reported by 'make print-modules'," \
           "expected the full engine — MODULES is probably mangled" >&2
      exit 1
    fi

    # And the other direction: an installed header with no module behind it
    # means the install rule and the module list have parted company.
    installed=$(cd "$out/mips64-elf/include/kiln" && ls *.h | sed 's/\.h$//' | sort)
    declared=$(printf '%s\n' $headers | sort)
    if ! diff <(echo "$installed") <(echo "$declared") >/dev/null; then
      echo "engine.nix: installed headers disagree with MODULES:" >&2
      diff <(echo "$installed") <(echo "$declared") >&2 || true
      exit 1
    fi

    echo "engine.nix: $n modules, headers and MODULES agree"
    runHook postInstallCheck
  '';

  meta = {
    description = "Kiln engine — Tiny3D 3D layer + rdpq 2D GUI layer";
    license = pkgs.lib.licenses.mpl20;
    platforms = pkgs.lib.platforms.linux;
  };
}
