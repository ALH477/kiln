# SPDX-License-Identifier: MIT
#
# nix/tools/unfloader.nix — N64-UNFLoader host-side tool.
#
# UNFLoader is a cross-flashcart loader and debug bridge. It speaks ED64
# (FT245R), SC64, 64drive, and Gopher64, and supports debugf() stdio over USB.
# The ED64 in particular has no other open-source Linux tool, so this is the
# fallback when a cart presents as 0403:6001 instead of the SC64's 0403:6014.
#
# Build notes:
#  * The Makefile lives in ./UNFLoader/ and expects g++ on Linux. Its CFLAGS
#    hardcode -I "$(PREFIX)/include", but the Nix-wrapped g++ already injects the
#    libftdi1/libusb include paths via NIX_CFLAGS_COMPILE, so we leave CFLAGS
#    alone and only override the install prefix and compiler name.
#  * libudev is needed by libusb for enumeration; ncursesw is used by the
#    interactive terminal UI.
{ pkgs, src }:

pkgs.stdenv.mkDerivation {
  pname = "unfloader";
  version = "2.11.0";
  inherit src;

  sourceRoot = "source/UNFLoader";

  nativeBuildInputs = [
    pkgs.pkg-config
  ];

  buildInputs = [
    pkgs.ncurses
    pkgs.libftdi1
    pkgs.libusb1
  ] ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [
    pkgs.udev
  ];

  makeFlags = [
    "PREFIX=$(out)"
    "CXX=${pkgs.stdenv.cc.targetPrefix}g++"
  ];

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    cp UNFLoader $out/bin/unfloader
    runHook postInstall
  '';

  doCheck = false;

  meta = {
    description = "Cross-flashcart N64 loader and USB stdio/debug bridge";
    homepage = "https://github.com/buu342/N64-UNFLoader";
    license = pkgs.lib.licenses.gpl3Only;
    mainProgram = "unfloader";
    platforms = pkgs.lib.platforms.linux;
  };
}
