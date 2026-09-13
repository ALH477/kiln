# SPDX-License-Identifier: MIT
#
# nix/checks/studio-manifest.nix — Kiln Studio's project manifest covers the
# repository: every pc-/web- build carries its record, every jump ROM's base is a
# ROM, every examples/ directory is built, every other package is a known tool,
# and the cheap-check list names real checks. tools/studio/manifest_check.py
# runs its rules on deliberately broken copies first, so a rule that cannot fire
# fails the check rather than passing it.
{ pkgs, manifest, examplesDir, checker, allow }:

pkgs.runCommand "check-studio-manifest"
{
  nativeBuildInputs = [ pkgs.python3Minimal ];
  manifestJson = builtins.toJSON manifest;
  passAsFile = [ "manifestJson" ];
  meta.description = "Kiln Studio's generated manifest agrees with the flake's packages and examples/";
}
  ''
    set -euo pipefail
    mkdir -p "$out"
    cp "$manifestJsonPath" "$out/manifest.json"
    python3 ${checker} "$out/manifest.json" ${examplesDir} ${allow} | tee "$out/report.txt"
  ''
