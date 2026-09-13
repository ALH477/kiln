# SPDX-License-Identifier: MIT
#
# nix/checks/poser-lag.nix — the poser previews the follow-through goblin.py bakes.
#
# tools/poser/src/lag.gen.js is generated from goblin.py's LAG table by
# tools/poser/gen_lag.py and committed. This regenerates it and fails on any
# difference, requires pose.js to take its LAG from that file rather than
# growing a copy again, and then proves the diff fires: one ear lagging a frame
# more in goblin.py must make the committed file stale.
{ pkgs, genLag, poserSrc, goblin }:

pkgs.runCommand "check-poser-lag"
{
  nativeBuildInputs = [ pkgs.python3 ];
  meta.description = "tools/poser/src/lag.gen.js matches goblin.py's LAG, and pose.js uses it";
}
  ''
    set -euo pipefail
    cp -r ${poserSrc} src
    cp ${goblin} goblin.py
    chmod -R u+w src goblin.py

    python3 ${genLag} --source goblin.py --out src/lag.gen.js --check

    grep -q 'from "./lag.gen.js"' src/pose.js \
      || { echo "pose.js no longer takes LAG from lag.gen.js"; exit 1; }
    if grep -nE '^(export )?const LAG\b' src/pose.js; then
      echo "pose.js defines its own LAG table again"; exit 1
    fi

    sed -i 's/"ear_l": 3/"ear_l": 4/' goblin.py
    grep -q '"ear_l": 4' goblin.py || { echo "the mutation did not apply — update this check"; exit 1; }
    if python3 ${genLag} --source goblin.py --out src/lag.gen.js --check >/dev/null; then
      echo "a changed LAG in goblin.py went unnoticed"; exit 1
    fi

    echo "poser-lag: ok" | tee "$out"
  ''
