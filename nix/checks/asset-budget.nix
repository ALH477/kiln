# SPDX-License-Identifier: MIT
#
# nix/checks/asset-budget.nix — a game's scenes, held to a declared cost.
#
# Exposed downstream as `kiln.lib.mkAssetBudgetCheck`, because the budget is a
# GAME's statement and the measurement is the ENGINE's. Kiln cannot know what
# scenes a game has; a game should not have to reimplement how many bytes of
# TMEM a 32x32 CI4 tile needs.
#
# ── What this is for, and what already covers the rest ─────────────────────
# rom.nix asserts a .z64's magic, title and total size. kiln-asset.nix
# round-trips the asset layer. level-vocab.nix keeps the entity vocabulary to
# one statement. kilnlib.report takes a `max_tris` ceiling. Each of those is
# about one artifact, or about the ROM as a whole.
#
# The numbers that decide whether a SCENE runs are sums, and nothing summed
# them: triangles and vertices submitted per frame, bytes resident in RDRAM,
# TMEM per material, mixer channels in use. In practice they end up in prose —
# "2,058 triangles for the set", "each under 1,088 B of TMEM", "the mixer has
# 16 SFX channels" — and prose does not fail a build. The result is not a
# crash but a screen quietly running at half its frame rate, which `nix build`
# cannot see.
#
# ── The two directions, and why the second one is the point ────────────────
# Downward: every scene's totals must fit its ceiling.
# Upward:   every asset the game ships must appear in at least one scene.
#
# The second is what makes this a gate rather than a report. Without it the
# check is opt-in, and the asset nobody thought to budget is precisely the one
# that pushes a screen over. That is not hypothetical: four generated props
# once reached a ROM and were placed on a level while being absent from every
# triangle ceiling and texture gate in the project, and nothing anywhere
# noticed.
#
# ── The budget's shape ─────────────────────────────────────────────────────
#   {
#     "platform": { "tmem_bytes": 4096, "mixer_channels": 16,
#                   "rdram_bytes": 3145728 },
#     "all_assets": [ { "kind": "model", "name": "island" }, ... ],
#     "scenes": {
#       "PLAY": {
#         "ceiling": { "tris": 3000, "verts": 3200, "resident_bytes": 900000 },
#         "assets": [ { "kind": "model", "name": "island" },
#                     { "kind": "texture", "name": "imp_hide",
#                       "format": "CI4" } ]
#       }
#     }
#   }
#
# A scene with no `ceiling` FAILS rather than passing, a ceiling naming a
# quantity nothing measures FAILS, and a missing `all_assets` FAILS. Every one
# of those defaults would otherwise report green while checking nothing, which
# is the failure mode this file is most concerned with.
#
# ── Proving it fires, which is the house rule ──────────────────────────────
# `tools/asset_budget.py selftest` exercises all nineteen assertions against a
# fixture, in both directions, and runs first below. If the selftest is the
# only thing that ever goes red, that is still the signal: it means the
# measurer stopped measuring.
{ pkgs
, name ? "asset-budget"
  # The game's budget.json.
, budget
  # name -> built model derivation (the directory holding share/gltf and
  # filesystem/**.t3dm). measure_model reads the intermediate glTF, because
  # that is the only place the SHIPPED triangle count is legible.
, models ? { }
  # name -> source PNG. Measured from the PNG rather than the .sprite on
  # purpose: libdragon's sprite container has changed shape more than once, a
  # palette index has not, and the existing per-texture gates in this repo
  # check the PNG too.
, textures ? { }
  # name -> source WAV. Channel count is load-bearing: the mixer plays a
  # stereo waveform across two ADJACENT channels, so a stereo bed counted as
  # one channel describes a ROM that asserts in mixer_poll a few seconds into
  # boot.
, audio ? { }
, tool ? ../../tools/asset_budget.py
}:

let
  stage = kind: set:
    pkgs.lib.concatStringsSep "\n"
      (pkgs.lib.mapAttrsToList (n: src: ''
        mkdir -p "root/${kind}"
        cp -rL ${src} "root/${kind}/${n}"
      '') set);
in
pkgs.runCommand "check-${name}"
{
  nativeBuildInputs = [ pkgs.python3 pkgs.python3Packages.pillow ];
  meta.description = "every scene fits its declared cost, and every asset is budgeted";
}
  ''
    set -euo pipefail

    # The selftest first. A measurer that has stopped measuring would
    # otherwise let every budget below pass for the wrong reason.
    echo "── the measurer proves itself ──"
    python3 ${tool} selftest

    echo
    echo "── staging the assets the budget names ──"
    ${stage "model" models}
    ${stage "texture" textures}
    ${stage "audio" audio}
    find root -maxdepth 2 -mindepth 2 | sort | sed 's/^/  /'

    echo
    python3 ${tool} check --budget ${budget} --root root

    mkdir -p "$out"
    cp ${budget} "$out/budget.json"
  ''
