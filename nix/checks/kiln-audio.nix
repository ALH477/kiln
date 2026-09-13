# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-audio.nix — kiln_music's player state, kiln_audio's tap,
# pitch as a ratio, and stereo SFX taking a channel pair.
#
# kiln_music_playing asked whether a track's FIRST mixer channel was playing,
# and libdragon's XM player stops any channel that has no sample on a tick, so a
# tune whose first column rests read as stopped. kiln_sound passed a pitch
# RATIO to a function taking Hz. kiln_sfx's voice steal picked the secondary
# half of a stereo voice and the console asserted. See kiln-audio-check.c.
#
# The .xm64 is a one-byte placeholder — the host player is bookkeeping only and
# checks the file exists. The two sounds are real converter output, because
# pitch is measured by how long the host mixer takes to play one out, and
# stereo by what the host mixer (mirroring libdragon's asserts) allows.
{ pkgs, target, sound, stereo }:

target.mkCheck {
  pname = "audiocheck";
  sources = [ ./kiln-audio-check.c ];
  env = "KILN_HOST_DFS=fs";
  preRun = ''
    mkdir -p fs/music
    cp -rL --no-preserve=mode ${sound}/filesystem/. fs/
    cp -L --no-preserve=mode ${stereo}/filesystem/*.wav64 fs/stereo.wav64
    printf 'x' > fs/music/tune.xm64
  '';
  meta.description = "kiln_music_playing follows the player, the tap sees every buffer, pitch is a ratio, stereo takes a pair";
}
