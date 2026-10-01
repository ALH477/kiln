# SPDX-License-Identifier: MIT
#
# nix/instruments.nix — the instrument library, built and held to its catalogue.
#
# dsp/instruments/catalogue.json is the ONE statement of what the library is: each
# instrument's family, kind, reference pitch, gate timing, whether it is a live
# voice and what cycle budget it declares. This file reads it and derives
# everything else from it, so adding an instrument is a `.dsp`, a catalogue entry
# and nothing in Nix — the same shape engine/modules.mk gives a module.
#
# Per instrument:
#   baked.<name>     VADPCM .wav64 via mkBakedInstrument (every instrument)
#   voices.<name>    a MIPS object via mkFaustVoice, for those marked `live`
#                    — which is where the no-libm, no-double and cycle gates
#                    apply, so `live: true` is a claim the build checks
#
# Per library:
#   all              every .wav64 in one filesystem/ — an asset a ROM can take whole
#   checks.*         what holds the library to its catalogue; see below
#
# ── What the checks are for ────────────────────────────────────────────
# mkBakedInstrument already refuses silence, < -40 dBFS and clipping. Those pass
# a clean 440 Hz sine that is supposed to be a bell and a "harp" that plays the
# wrong note, because loudness is not identity. The catalogue's `kind` is the
# claim, and tools/instruments/check.py is what measures it: a harmonic
# instrument's autocorrelation pitch must land on the requested frequency at the
# reference note AND an octave up, an inharmonic one must move its whole spectrum
# when `freq` moves, a gated one must release, and nothing may carry DC or fold
# its upper partials back below Nyquist. `instruments-live` then renders the
# SINGLE-PRECISION, ONE-SAMPLE build of each live voice — the code that actually
# ships in a ROM — through the same measurements, because a voice that is right
# in double and wrong in float is exactly what the offline renderer cannot see.
{ pkgs, faust }:

let
  lib = pkgs.lib;

  catalogueFile = ../dsp/instruments/catalogue.json;
  catalogue = builtins.fromJSON (builtins.readFile catalogueFile);
  insts = catalogue.instruments;
  sampleRate = catalogue.sampleRate;

  # The primitives library, as its own store path, handed to faust as -I.
  kilnLib = builtins.path { path = ../dsp/lib; name = "kiln-dsp-lib"; };
  includes = [ kilnLib ];

  srcOf = name: ../dsp/instruments + "/${name}.dsp";
  dspDir = builtins.path { path = ../dsp/instruments; name = "kiln-instruments"; };
  tools = ../tools/instruments;

  # `freq` is only passed when the instrument has the slider; a noise drum
  # without one would otherwise fail with "no such parameter".
  paramsOf = i: (i.params or { }) // lib.optionalAttrs (i.freqParam or true) { freq = i.freq; };

  liveInsts = lib.filterAttrs (_: i: i.live or false) insts;

  renderers = lib.mapAttrs
    (name: _: faust.mkOfflineRenderer { inherit name includes; src = srcOf name; })
    insts;

  baked = lib.mapAttrs
    (name: i: faust.mkBakedInstrument {
      inherit name includes sampleRate;
      src = srcOf name;
      duration = i.duration;
      params = paramsOf i;
      gate = { param = "gate"; on = i.gate.on; off = i.gate.off; };
      # A stereo wav64 occupies TWO mixer channels (CLAUDE.md, "The audio
      # layer"), so an instrument is baked mono unless the catalogue says it is
      # genuinely wider; check.py holds the render to that claim.
      mono = !(i.stereo or false);
    })
    insts;

  voices = lib.mapAttrs
    (name: i: faust.mkFaustVoice {
      inherit name includes sampleRate;
      src = srcOf name;
      budget = i.budget or 500;
    })
    liveInsts;

  # The live build, natively: the architecture file, the voice, and a small
  # driver that calls the architecture's public entry points. Same C, the host's
  # compiler, so what it proves is the single-precision / one-sample behaviour,
  # not the VR4300's.
  liveRenderers = lib.mapAttrs
    (name: _: pkgs.runCommandCC "instrument-live-${name}"
      { nativeBuildInputs = [ pkgs.faust pkgs.which ]; }
      ''
        faust -lang c -single -os -ftz 1 -I ${kilnLib} -cn ${name} \
              -a ${../dsp/arch/libdragon_mixer.c} ${srcOf name} -o live.c
        mkdir -p $out/bin
        $CC -O2 -ffast-math -std=gnu17 -DFAUST_NAME=${name} \
            -DFAUST_N64_SAMPLE_RATE=${toString sampleRate} \
            -I${pkgs.faust}/include live.c ${tools}/live_render.c \
            -o $out/bin/render-${name} -lm
      '')
    liveInsts;

  join = n: attrs: pkgs.symlinkJoin { name = n; paths = lib.attrValues attrs; };

  names = lib.concatStringsSep "," (lib.attrNames insts);
  liveNames = lib.concatStringsSep "," (lib.attrNames liveInsts);
in
{
  inherit catalogue baked voices renderers liveRenderers;

  # Every baked instrument in one filesystem/, for a ROM that wants the lot.
  all = join "kiln-instruments-baked" baked;

  checks = {
    # The catalogue and the sources agree: nothing unlisted, nothing missing,
    # standard controls present, a `live` source reaches only libm-free
    # namespaces. Cheap, so it runs first and names the mistake at the line.
    instruments-lint = pkgs.runCommand "check-instruments-lint" { nativeBuildInputs = [ pkgs.python3 ]; } ''
      python3 ${tools}/check.py --lint ${catalogueFile} ${dspDir}
      touch $out
    '';

    # INDEX.md is generated; a hand edit, or a new instrument with no
    # regeneration, is a diff.
    instruments-index = pkgs.runCommand "check-instruments-index" { nativeBuildInputs = [ pkgs.python3 ]; } ''
      python3 ${tools}/index.py ${catalogueFile} ${dspDir} > fresh.md
      if ! diff -u ${../dsp/instruments/INDEX.md} fresh.md; then
        echo "dsp/instruments/INDEX.md is stale: python3 tools/instruments/index.py dsp/instruments/catalogue.json dsp/instruments > dsp/instruments/INDEX.md" >&2
        exit 1
      fi
      touch $out
    '';

    # Every instrument, rendered at full quality and measured.
    instruments-render = pkgs.runCommand "check-instruments-render"
      { nativeBuildInputs = [ pkgs.python3 ]; }
      ''
        mkdir -p $out
        python3 ${tools}/check.py ${catalogueFile} ${join "kiln-instrument-renderers" renderers}/bin --keep $out
      '';

    # The voices that ship in a ROM, rendered the way a ROM renders them.
    instruments-live = pkgs.runCommand "check-instruments-live"
      { nativeBuildInputs = [ pkgs.python3 ]; }
      ''
        mkdir -p $out
        python3 ${tools}/check.py ${catalogueFile} ${join "kiln-instrument-live" liveRenderers}/bin \
          --only ${liveNames} --keep $out
      '';

    # Forces every live voice through mkFaustVoice's three gates — no libm in the
    # object, no double-precision instruction, frame() inside its declared cycle
    # budget — and every instrument through audioconv64. A `live: true` that
    # lied fails HERE, by name.
    instruments-voices = join "kiln-instrument-voices" voices;
    instruments-baked = join "kiln-instrument-baked" baked;
  };
}
