# SPDX-License-Identifier: MIT
#
# nix/udev.nix — nixosModules.n64-flashcart
#
# Without this, sc64deployer can enumerate the cart but cannot claim the USB
# interface, so `./dev deploy` fails with a permissions error unless run as
# root. Running a deploy loop as root is the wrong answer.
#
# The SummerCart64 presents as an FTDI FT232H (VID 0403, PID 6014) with the USB
# product description "SC64" — the same triple sc64deployer matches on in
# `sw/deployer/src/sc64/link.rs`. Note that PID 6014 is the generic FT232H ID,
# not SC64-specific, so this rule also grants access to any other FT232H board
# on the machine; that is the same trade-off upstream's own rules make.
#
# Usage, in your NixOS configuration:
#
#   imports = [ kiln.nixosModules.n64-flashcart ];
#   programs.n64-flashcart.enable = true;
#
# then re-login (for the group) or `sudo udevadm control --reload`.
{ config, lib, pkgs, ... }:

let
  cfg = config.programs.n64-flashcart;
in
{
  options.programs.n64-flashcart = {
    enable = lib.mkEnableOption "udev rules for N64 flashcart USB access";

    group = lib.mkOption {
      type = lib.types.str;
      default = "plugdev";
      description = ''
        Group granted access to the cart. Members need to be in it, and the
        group must exist — set `users.groups.<name> = {}` if it does not.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    services.udev.extraRules = ''
      # SummerCart64 (FTDI FT232H) — ROM upload, debugf stdio, IS-Viewer64
      SUBSYSTEM=="usb", ATTR{idVendor}=="0403", ATTR{idProduct}=="6014", MODE="0660", GROUP="${cfg.group}", TAG+="uaccess"
      # The same device once the ftdi_sio driver binds it as a tty.
      SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6014", MODE="0660", GROUP="${cfg.group}", TAG+="uaccess"
      # EverDrive 64 / ED64 (FTDI FT245R) — ROM upload + debugf stdio via UNFLoader.
      # Some revisions use different FTDI chips; run `lsusb` and extend the rule
      # if yours differs.
      SUBSYSTEM=="usb", ATTR{idVendor}=="0403", ATTR{idProduct}=="6001", MODE="0660", GROUP="${cfg.group}", TAG+="uaccess"
      SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6001", MODE="0660", GROUP="${cfg.group}", TAG+="uaccess"
    '';
  };
}
