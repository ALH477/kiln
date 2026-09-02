# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-wav64.nix — the host makes sound, and it is the right sound.
#
# The asset is built by the SAME assetLib.mkSound the ROMs use, so this is
# audioconv64's real output — VADPCM, big-endian, with the codebook and the
# padding-to-32-samples that the container's own header does not mention. A
# hand-made input would test the check.
#
# What it measures is RMS, not sample count. plat/host/src/host_wav64.c leans
# on libdragon's own vendored decoder rather than a reimplementation precisely
# because a decoder can be structurally perfect and acoustically wrong, and
# silence is the least attributable failure in the whole audio path: a missing
# asset, a stopped channel, a zero volume and a broken codebook all present
# identically. So it asserts the samples have energy, that the mixer preserves
# it, and that pan 0.0 is hard left — libdragon's convention, and backwards is
# the kind of bug nobody reports because it merely sounds wrong.
{ pkgs, target, sound }:

target.mkCheck {
  pname = "wav64check";
  sources = [ ./kiln-wav64-check.c ];
  args = "rom:/sfx/blip.wav64";
  env = "KILN_HOST_DFS=fs";
  meta.description = "a real .wav64 decodes, mixes and pans on the host";
  preRun = ''
    mkdir -p fs
    cp -rL --no-preserve=mode ${sound}/filesystem/. fs/
    find fs -type f | sed 's/^/  /'
  '';
}
