#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# stage.sh — put everything the poser needs into tools/poser/data/.
#
# The .gltf the editor loads is not a special export: it is the SAME
# intermediate nix/blender.nix already keeps at $out/share/gltf/ for
# debugging, which is the whole reason the editor can be sure it is looking
# at what the ROM will contain. The source keyframes come out of the model
# script itself via anim_io.py. Neither is authored twice.
set -euo pipefail
cd "${M64_REPO:-$PWD}"
mkdir -p tools/poser/data

MODELS=("${@:-dank sparky moss glimmer goblin}")
read -ra MODELS <<< "${MODELS[*]}"

for m in "${MODELS[@]}"; do
  echo "── $m ──"
  out=$(nix build ".#model-$m" --no-link --print-out-paths)
  cp -f "$out/share/gltf/$m.gltf" "$out/share/gltf/$m.bin" tools/poser/data/
  chmod u+w "tools/poser/data/$m.gltf" "tools/poser/data/$m.bin"
  python3 tools/blender/anim_io.py dump goblin "$m" tools/poser/data | grep -v WELD
done

echo
python3 tools/poser/verify.py "${MODELS[@]}" | grep -v WELD
