# SPDX-License-Identifier: MIT
#
# examples/live-voice. ROM only: the live voice is a MIPS object from
# mkFaustVoice, so there is no host build. The jump ROM plucks the live and
# baked voices alternately forever, so any capture has both strings ringing.
ctx: with ctx;
mkJumpRoms args.liveVoiceArgs [ "AB" ]
