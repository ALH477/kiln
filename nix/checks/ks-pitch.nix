# SPDX-License-Identifier: MIT
#
# nix/checks/ks-pitch.nix — dsp/ks.dsp's `freq` slider changes the pitch.
#
# It did not, for as long as the file existed: pm.ks takes a string LENGTH in
# metres clamped to 3, so 220 "Hz" rendered the longest string there is, and
# 220, 440 and 880 produced byte-identical files. The excitation was the gate
# itself, a step, so what came out was a decaying DC offset with no measurable
# pitch at all. mkBakedInstrument's gates (silence, too quiet, clipping) all
# passed that, because loudness is not pitch — this is the gate about the one
# thing a pitched instrument is for.
#
# Renders with the same full-quality host renderer ks-baked uses, at two
# pitches an octave apart, and requires each to measure within 5% of what was
# asked, to be periodic, and to carry no step-like DC.
{ pkgs, renderer, name ? "ksvoice" }:

pkgs.runCommand "check-ks-pitch" { nativeBuildInputs = [ renderer pkgs.python3 ]; } ''
  set -euo pipefail
  fails=0
  for f in 220 440; do
    render-${name} -o ks_$f.wav -r 32000 -d 1.0 -p freq=$f -p gain=0.25 -g gate:0.0:0.02 >/dev/null
    python3 ${./ks_pitch.py} ks_$f.wav $f || fails=$((fails + 1))
  done
  if cmp -s ks_220.wav ks_440.wav; then
    echo "  FAIL: freq=220 and freq=440 render byte-identical files" >&2
    fails=$((fails + 1))
  fi
  if [ "$fails" -ne 0 ]; then
    echo "ks-pitch: $fails failure(s)" >&2
    exit 1
  fi
  mkdir -p $out && cp ks_220.wav ks_440.wav $out/
''
