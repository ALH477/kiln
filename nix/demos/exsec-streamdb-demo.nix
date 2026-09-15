# SPDX-License-Identifier: MIT
#
# examples/exsec-streamdb-demo: the DONE jump ROM, which holds the reveal at its
# end so one capture shows every check, the verdict and the typed document.
# No host build: the generated reader asserts 32-bit addresses, and the ROM
# runs it on a libdragon kthread.
ctx: with ctx;
mkJumpRoms args.exsecStreamdbDemoArgs [ "DONE" ]
