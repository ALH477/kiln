# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-console.nix — the on-screen console takes its input through kiln_input.
#
# It read the joypad directly, so scripted input (attract tapes, jump ROMs) could
# never open it. See kiln-console-check.c.
{ pkgs, target }:

target.mkCheck {
  pname = "consolecheck";
  sources = [ ./kiln-console-check.c ];
  meta.description = "kiln_console opens from a real pad and from a kiln_input tape";
}
