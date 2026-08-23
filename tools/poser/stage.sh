#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# stage.sh — put everything the poser needs into tools/poser/data/.
#
# The .gltf the editor loads is not a special export: it is the SAME
# intermediate nix/blender.nix already keeps at $out/share/gltf/ for
# debugging, which is the whole reason the editor can be sure it is looking
# at what the ROM will contain. The source keyframes come out of the model
# script itself via anim_io.py. Neither is authored twice.
#
# ── Rig-JSON characters moved out with PetaByte Madness ────────────────
# This used to also stage two rig-JSON characters (Horner, the machine
# centaur) whose clips arrive in assets/rig/*.json rather than as code —
# anim_io.capture() can't recover those from a stubbed model script the way
# it does for the goblin family, so anim_io reads the rig JSON directly for
# them instead. Both characters, their rigs, and the `model-horner`/
# `model-centaur` flake packages this script built them from have since
# moved to PetaByte Madness's own repo along with the game; the rig-JSON
# path in anim_io.py (`--rig <path>`) is still there for a game with its own
# such characters to point at its own rig JSON.
#
#   ./tools/poser/stage.sh                # everything
#   ./tools/poser/stage.sh dank           # one
set -euo pipefail
cd "${KILN_REPO:-$PWD}"
mkdir -p tools/poser/data

# model -> "script rig-json-or-'-'"
declare -A SPEC=(
  [dank]="goblin -"
  [sparky]="goblin -"
  [moss]="goblin -"
  [glimmer]="goblin -"
  [goblin]="goblin -"
)

DEFAULT_MODELS="dank sparky moss glimmer goblin"
read -ra MODELS <<< "${*:-$DEFAULT_MODELS}"

# Only the code-clip characters can be checked by verify.py — it compares the
# ideal eased curve against the exported one, and the rig-JSON characters have
# no easing to compare (their keys are already the exporter's own). Collected
# separately rather than filtered inside verify.py, so verify.py keeps saying
# exactly what it proves.
VERIFIABLE=()

for m in "${MODELS[@]}"; do
  spec="${SPEC[$m]:-}"
  if [ -z "$spec" ]; then
    echo "stage.sh: unknown model '$m' (have: ${!SPEC[*]})" >&2
    exit 1
  fi
  read -r script rig <<< "$spec"

  echo "── $m ($script${rig:+, $rig}) ──"
  out=$(nix build ".#model-$m" --no-link --print-out-paths)
  cp -f "$out/share/gltf/$m.gltf" "$out/share/gltf/$m.bin" tools/poser/data/
  chmod u+w "tools/poser/data/$m.gltf" "tools/poser/data/$m.bin"

  if [ "$rig" = "-" ]; then
    python3 tools/blender/anim_io.py dump "$script" "$m" tools/poser/data \
      | grep -v WELD
    VERIFIABLE+=("$m")
  else
    python3 tools/blender/anim_io.py dump "$script" "$m" tools/poser/data \
      --rig "$rig" | grep -v WELD
    echo "  (rig JSON: keys are the exporter's own, so no easing to verify —"
    echo "   the convention is proved by ${script}.py --selftest instead)"
  fi
done

if [ ${#VERIFIABLE[@]} -gt 0 ]; then
  echo
  python3 tools/poser/verify.py "${VERIFIABLE[@]}" | grep -v WELD
fi
