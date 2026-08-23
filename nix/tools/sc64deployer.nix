# SPDX-License-Identifier: MIT
#
# nix/tools/sc64deployer.nix — the SummerCart64 host-side tool.
#
# This is the primary hardware path. Per report §7 the SC64 is the flashcart to
# use (open-source, ~23.8 MiB/s USB, microSD, GDB, IS-Viewer64, well-documented
# command protocol), and §6 identifies its AUX register + `AUX_WRITE` command as
# the PC<->N64 message link the SSHitunneller! control tunnel would ride on.
#
# Build notes:
#  * `build.rs` compiles FatFs C sources from `../bootloader/src/fatfs/` and
#    runs bindgen over them. So the whole repo has to stay unpacked —
#    `buildAndTestSubdir` rather than `sourceRoot`, or the relative path breaks.
#  * `libftdi1-sys` and `libusb1-sys` are pulled with their `vendored` features,
#    so they compile from source and need no system libftdi/libusb. They do
#    need a working C toolchain and pkg-config.
{ pkgs, src }:

pkgs.rustPlatform.buildRustPackage {
  pname = "sc64deployer";
  version = "2.20.2";
  inherit src;

  # cargoRoot tells the vendoring hook where Cargo.lock lives; buildAndTestSubdir
  # is where cargo actually runs. Both are needed: the crate is in a subdirectory
  # but its build.rs reaches UP to ../bootloader/src/fatfs, so the whole repo has
  # to stay unpacked and we cannot just use sourceRoot.
  cargoRoot = "sw/deployer";
  buildAndTestSubdir = "sw/deployer";

  cargoLock = {
    lockFile = "${src}/sw/deployer/Cargo.lock";
  };

  nativeBuildInputs = [
    pkgs.pkg-config
    # Supplies LIBCLANG_PATH and the include flags bindgen needs.
    pkgs.rustPlatform.bindgenHook
  ];

  buildInputs = [
    pkgs.libusb1
  ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [
    pkgs.udev
  ];

  # There is no hardware in the sandbox, so anything beyond compilation would
  # fail for reasons unrelated to the build.
  doCheck = false;

  meta = {
    description = "SummerCart64 loader and control software";
    homepage = "https://github.com/Polprzewodnikowy/SummerCart64";
    license = pkgs.lib.licenses.gpl3Only;
    mainProgram = "sc64deployer";
    platforms = pkgs.lib.platforms.linux;
  };
}
