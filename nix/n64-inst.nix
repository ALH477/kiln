# SPDX-License-Identifier: MIT
#
# nix/n64-inst.nix — the merged $N64_INST prefix.
#
# libdragon and everything layered on top of it (Tiny3D, and any future
# library following the same convention) all expect to be installed into ONE
# prefix: headers under `$N64_INST/mips64-elf/include`, archives under
# `$N64_INST/mips64-elf/lib`, makefile fragments under `$N64_INST/include`,
# host tools under `$N64_INST/bin`. Tiny3D's `t3d-inst.mk` literally does
# `N64_LDFLAGS := -lt3d $(N64_LDFLAGS)`, which only resolves if libt3d.a sits
# beside libdragon.a.
#
# Nix store paths are immutable, so each library installs into its own output
# with that layout and we lndir them together here. symlinkJoin merges nested
# directories, so `mips64-elf/lib` ends up containing both archives.
#
# Note this is deliberately NOT merged with the toolchain: `n64.mk` keeps
# `N64_GCCPREFIX` separate from `N64_INST` precisely so a toolchain installed
# elsewhere can be used, which is our situation. Two variables, two paths.
{ pkgs, libdragon, extraLibs ? [ ] }:

pkgs.symlinkJoin {
  name = "n64-inst";
  paths = [ libdragon ] ++ extraLibs;

  passthru = { inherit libdragon extraLibs; };

  meta = {
    description = "merged N64_INST prefix (libdragon + layered libraries)";
    platforms = pkgs.lib.platforms.linux;
  };
}
