# SPDX-License-Identifier: MIT
#
# nix/dev-image.nix — NixOS config for packages.dev-image (nix/... see flake.nix)
#
# A container a collaborator can `nix develop`/`nix build`/`./dev` *inside*,
# not a pre-baked snapshot of this flake's own devShell. Deliberately does
# NOT duplicate toolchain/n64Inst/gnumake/faust/ares from devShells.default
# (flake.nix) — those come from running this flake's own `nix develop`
# against a cloned checkout of the repo, exactly as on a native NixOS
# machine, so there is one source of truth for the Kiln toolchain and zero
# drift between "what the image ships" and "what the flake's devShell
# defines". This image only needs to provide a working Nix (with flakes),
# git, Claude Code, and Tailscale (for the private registry + private git
# delivery over the tailnet — see the plan this shipped with).
#
# No GUI/GPU packages (ares, hyprland, ...): `./dev shot`/`./dev rec` require
# a live Hyprland/Wayland session and a real Vulkan surface, which no
# headless container can provide. Visual ROM testing happens outside this
# image, in a native emulator on the host.
{ config, lib, pkgs, claude-code-nix, ... }:

{
  nix.settings = {
    experimental-features = [ "nix-command" "flakes" ];
    trusted-users = [ "dev" ];
  };

  services.tailscale.enable = true;

  users.users.dev = {
    isNormalUser = true;
    home = "/home/dev";
    extraGroups = [ "wheel" ];
  };
  security.sudo.wheelNeedsPassword = false;

  environment.systemPackages = [
    claude-code-nix.packages.${pkgs.system}.default
    pkgs.git
    pkgs.openssh
    pkgs.ripgrep
    pkgs.cacert
  ];

  system.stateVersion = "24.11";
}
