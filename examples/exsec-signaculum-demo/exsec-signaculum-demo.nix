# SPDX-License-Identifier: MIT
#
# DRAFT — DELIBERATELY NOT WIRED INTO flake.nix YET.
#
# This is what examples/exsec-signaculum-demo's flake entry would look like,
# kept here as an untracked-by-flake text file so landing the demo does not
# require touching flake.nix in the same change as the engine module. When it
# does land:
#
#   1. In flake.nix, next to exsecStreamdbDemoArgs:
#
#        signaculumExsg = assetLib.mkRawAsset {
#          name = "signaculum";
#          src = ./examples/exsec-signaculum-demo/filesystem/signaculum.exsg;
#          dest = "";            # rom:/signaculum.exsg, main.c's EXSG_PATH
#          extension = "exsg";
#          compress = 0;         # the blob is the stream; mkasset -c 0 is a copy
#        };
#
#        exsecSignaculumDemoArgs = {
#          name = "exsec-signaculum-demo";
#          src = ./examples/exsec-signaculum-demo;
#          romTitle = "Kiln Exsecutor Signaculum";
#          assets = [ signaculumExsg ];
#        };
#
#        exsec-signaculum-demo = mkN64Rom exsecSignaculumDemoArgs;
#
#      (...and add exsecSignaculumDemoArgs to the `args` inherit list so this
#      file's jump-ROM context can see it — none defined yet, there is no
#      second state worth jumping to.)
#
#   2. Move this file to nix/demos/exsec-signaculum-demo.nix. With no jump
#      ROMs and no host build (the ROM runs the render on a libdragon kthread
#      and the mips64-row generated unit asserts 32-bit addresses), the merged
#      attribute set is empty — the file exists so the wiring above has one
#      home when a jump ROM or check arrives. A first honest check for it:
#      build the ROM, boot it in Ares with ISViewer stdout, grep for
#      "signaculum: AGREE" — the shape nix/checks/*.nix capture checks
#      already use, but Ares needs a compositor, so CI-form is an open
#      question (EMULATOR verification is not part of nix flake check — see
#      CLAUDE.md, "Not yet built").
#
#   3. Drop the ENGINE_DIR / SOFT3D_OBJ local-link block from this demo's
#      Makefile: once nix/engine.nix reinstalls libkiln with modules.mk's
#      kiln_soft3d entry, the prefix archive carries the module and -lkiln
#      resolves it with no local build.

ctx: with ctx; {
}
