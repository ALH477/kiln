# SPDX-License-Identifier: MIT
#
# assets-demo's jump ROM: boots on the interceptor and stays there, so
# `./dev shot` captures the hero model with no attract tape switching it away.
#
# No pc-* build: the logo is drawn with rdpq_sprite_blit, which plat/host does
# not provide, and a host branch in the example is not allowed.
ctx: with ctx;
mkJumpRoms args.assetsDemoArgs [ "SHIP" ]
