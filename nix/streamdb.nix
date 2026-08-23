# SPDX-License-Identifier: MIT
#
# nix/streamdb.nix — StreamDB embedded reader, built for the N64.
#
# Data management for the engine: a single-file, CRC-checked, suffix-indexed
# asset container read directly out of ROM. See streamdb-embedded/README.md for
# why this is a separate implementation rather than a port of the upstream C
# edition (short version: pthreads, flock, fsync and a 1 KB-per-node trie).
#
# Installed with the same layout as libdragon/Tiny3D/libkiln so it merges into
# the one $N64_INST prefix and a ROM links it with -lstreamdb_emb.
#
# Built against libdragon alone (not n64InstBase): streamdb-embedded needs only
# libdragon's n64.mk and the DFS backend's <libdragon.h>. Keeping it decoupled
# from tiny3d lets it land in n64InstBase's extraLibs, which the engine needs
# because kiln_asset.c includes <streamdb_embedded.h>. See flake.nix for the
# build-order: libdragon → streamdb-emb → n64InstBase (+tiny3d) → engine.
{ pkgs, src, toolchain, libdragon }:

pkgs.stdenv.mkDerivation {
  pname = "streamdb-embedded";
  version = "0.1.0";
  inherit src;

  nativeBuildInputs = [ toolchain libdragon pkgs.gnumake pkgs.which ];
  hardeningDisable = [ "all" ];
  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    export N64_INST="${libdragon}"
    export N64_GCCPREFIX="${toolchain}"
    make -j"$NIX_BUILD_CORES"
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    export N64_INST="${libdragon}"
    export N64_GCCPREFIX="${toolchain}"
    mkdir -p $out/include $out/mips64-elf/lib $out/mips64-elf/include/streamdb
    make install INSTALLDIR="$out"
    runHook postInstall
  '';

  dontStrip = true;
  dontPatchELF = true;

  doInstallCheck = true;
  installCheckPhase = ''
    for f in include/streamdb.mk \
             mips64-elf/lib/libstreamdb_emb.a \
             mips64-elf/include/streamdb/streamdb_embedded.h; do
      [ -e "$out/$f" ] || { echo "streamdb.nix: missing $f" >&2; exit 1; }
    done
  '';

  meta = {
    description = "StreamDB v3 reader for bare-metal N64";
    license = pkgs.lib.licenses.lgpl21Plus;
    platforms = pkgs.lib.platforms.linux;
  };
}
