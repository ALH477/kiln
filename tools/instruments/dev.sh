#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# dev.sh — the fast loop for writing an instrument: no Nix build, no ROM.
#
#   tools/instruments/dev.sh [name ...]      render + check (all if none given)
#   tools/instruments/dev.sh --syms [name]   undefined symbols of the LIVE build
#                                            (any libm name here fails nix build)
#   tools/instruments/dev.sh --live [name]   render the LIVE (single-precision, one-sample)
#                                            build of each live-marked instrument and check
#                                            it like the baked one
#   tools/instruments/dev.sh --budget [name] a PROXY for nix/faust.nix's cycle gate:
#                                            the same weights (div 29, mul 5, add/sub 3,
#                                            other FP 1) applied to the x86 frame().
#                                            Not the gate — the gate reads the MIPS object
#                                            in `nix build` — but within ~20% in practice,
#                                            which is enough to know BEFORE a 40-minute
#                                            toolchain build whether a voice is anywhere near.
#
# Needs `faust` and a C compiler on PATH (nix develop has both). Renders land in
# ${KILN_INSTR_OUT:-.instruments}/ so a person can open them in an editor.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${KILN_INSTR_OUT:-.instruments}
FINC=$(dirname "$(dirname "$(command -v faust)")")/include
mkdir -p "$OUT/bin"
syms=0; budget=0; live=0; buildonly=0
[ "${1:-}" = "--build-only" ] && { buildonly=1; shift; }
[ "${1:-}" = "--live" ] && { live=1; shift; }
[ "${1:-}" = "--syms" ] && { syms=1; shift; }
[ "${1:-}" = "--budget" ] && { budget=1; syms=1; shift; }
names=("$@")
if [ $live = 1 ]; then
  [ ${#names[@]} -eq 0 ] && mapfile -t names < <(python3 -c "import json;c=json.load(open('dsp/instruments/catalogue.json'))['instruments'];print('\n'.join(k for k,v in c.items() if v.get('live')))")
  for n in "${names[@]}"; do
    faust -lang c -single -os -ftz 1 -I dsp/lib -cn "$n" -a dsp/arch/libdragon_mixer.c "dsp/instruments/$n.dsp" -o "$OUT/live_$n.c"
    cc -O2 -ffast-math -std=gnu17 -DFAUST_NAME="$n" -DFAUST_N64_SAMPLE_RATE=32000 -I"$FINC" \
       "$OUT/live_$n.c" tools/instruments/live_render.c -o "$OUT/bin/render-$n" -lm
  done
  python3 tools/instruments/check.py dsp/instruments/catalogue.json "$OUT/bin" --keep "$OUT/wav_live" --only "$(IFS=,; echo "${names[*]}")"
  exit
fi
[ ${#names[@]} -eq 0 ] && mapfile -t names < <(python3 -c "import json;print('\n'.join(json.load(open('dsp/instruments/catalogue.json'))['instruments']))")
for n in "${names[@]}"; do
  src=dsp/instruments/$n.dsp
  if [ $syms = 1 ]; then
    faust -lang c -single -os -ftz 1 -I dsp/lib -cn "$n" -a dsp/arch/libdragon_mixer.c "$src" -o "$OUT/live_$n.c"
    cc -O2 -ffast-math -std=gnu17 -DFAUST_NAME="$n" -DFAUST_N64_SAMPLE_RATE=32000 -I"$FINC" -c "$OUT/live_$n.c" -o "$OUT/live_$n.o"
    if [ $budget = 1 ]; then
      objdump -d --no-show-raw-insn "$OUT/live_$n.o" | awk -v fname="frame$n" '
        $0 ~ "<" fname ">:" { in_f = 1; next }
        in_f && /^$/ { in_f = 0 }
        in_f && /\t(div|sqrt)ss/                          { c += 29 }
        in_f && /\tmulss/                                 { c += 5 }
        in_f && /\t(add|sub)ss/                           { c += 3 }
        in_f && /\t(max|min|cmp[a-z]*|andn?|xor|or)ps|\t(max|min|cmp[a-z]*)ss|\tcvt|\tsqrtss/ { c += 1 }
        END { printf "%-14s ~%d weighted cycles/sample (proxy)\n", "'"$n"':", c }'
    else
      echo "$n: $(nm -u "$OUT/live_$n.o" | awk '{print $2}' | sort -u | tr '\n' ' ')"
    fi
    continue
  fi
  faust -lang c -double -ftz 1 -I dsp/lib -cn "$n" -a dsp/arch/offline_ref.c "$src" -o "$OUT/$n.c"
  cc -O2 -std=gnu17 -DFAUST_NAME="$n" -I"$FINC" "$OUT/$n.c" -o "$OUT/bin/render-$n" -lm
done
[ $syms = 1 ] || [ $buildonly = 1 ] || python3 tools/instruments/check.py dsp/instruments/catalogue.json "$OUT/bin" --keep "$OUT/wav" --only "$(IFS=,; echo "${names[*]}")"
