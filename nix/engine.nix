# SPDX-License-Identifier: MPL-2.0
#
# nix/engine.nix — the M64 engine: 3D on Tiny3D, 2D GUI on rdpq.
#
# Packaged exactly like Tiny3D (same $N64_INST layout, own store path, merged
# by n64-inst.nix) so a ROM links it with a plain `-lm64` and nothing here is
# special-cased.
#
# `n64InstBase` must be a prefix containing BOTH libdragon and Tiny3D: the
# engine's headers include <libdragon.h> and <t3d/t3d.h>. That is why the flake
# builds the prefix in two stages — libdragon+tiny3d first to compile against,
# then the engine merged in on top for ROMs to consume.
{ pkgs, src, toolchain, n64InstBase }:

pkgs.stdenv.mkDerivation {
  pname = "m64-engine";
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
    mkdir -p $out/include $out/mips64-elf/lib $out/mips64-elf/include/m64
    make install INSTALLDIR="$out"
    runHook postInstall
  '';

  dontStrip = true;
  dontPatchELF = true;

  doInstallCheck = true;
  installCheckPhase = ''
    for f in include/m64.mk \
             mips64-elf/lib/libm64.a \
             mips64-elf/include/m64/m64_engine.h \
             mips64-elf/include/m64/m64_gui.h \
             mips64-elf/include/m64/m64_actor.h \
             mips64-elf/include/m64/m64_room.h \
             mips64-elf/include/m64/m64_asset.h \
             mips64-elf/include/m64/m64_audio.h \
             mips64-elf/include/m64/m64_camera.h \
             mips64-elf/include/m64/m64_skel.h \
             mips64-elf/include/m64/m64_input.h \
             mips64-elf/include/m64/m64_clip.h \
             mips64-elf/include/m64/m64_dict.h \
             mips64-elf/include/m64/m64_map.h \
             mips64-elf/include/m64/m64_surface.h \
             mips64-elf/include/m64/m64_sound.h \
             mips64-elf/include/m64/m64_event.h \
             mips64-elf/include/m64/m64_target.h \
             mips64-elf/include/m64/m64_player.h; do
      [ -e "$out/$f" ] || { echo "engine.nix: missing $f" >&2; exit 1; }
    done
  '';

  meta = {
    description = "M64 engine — Tiny3D 3D layer + rdpq 2D GUI layer";
    license = pkgs.lib.licenses.mpl20;
    platforms = pkgs.lib.platforms.linux;
  };
}
