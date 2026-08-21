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
#
# ── Two kinds of character ─────────────────────────────────────────────
# This used to stage the goblin family and nothing else, and the reason was not
# a decision: anim_io.capture() ran the model script with kilnlib stubbed, which
# works for goblin.py (its clips are code) and returns an EMPTY LIST for
# centaur.py and horner.py (their clips arrive in assets/rig/*.json and their
# build_armature does real bpy work the stubs cannot fake). So the two
# characters PetaByte Madness actually animates had no editor, and their timing
# got tuned by editing C literals and rebuilding.
#
# anim_io now reads the rig JSON directly for those — it IS the source-keyframe
# form, so nothing is recovered from itself. The table below is the only thing
# that differs per character.
#
#   ./tools/poser/stage.sh                # everything
#   ./tools/poser/stage.sh horner         # one
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
  [horner]="horner PetaByte-Madness/assets/rig/horner.json"
  [centaur]="centaur PetaByte-Madness/assets/rig/machine_centaur.json"
)

DEFAULT_MODELS="dank sparky moss glimmer goblin horner centaur"
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
