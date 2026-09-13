# SPDX-License-Identifier: MIT
#
# nix/checks/studio-api.nix — Kiln Studio's server against its own attack suite.
#
# tools/studio/tests/api_test.py runs the real server.py (on a loopback port the
# sandbox allows) against a fake `nix` and a fixture manifest: auth, Host and
# Origin checks, Tailscale identity only from the proxy address, the job argv
# whitelist, SSE ordering and resume, cancel killing the process tree, static
# path containment. Then it restarts the server with each guard switched off and
# requires the suite to fail every time — so a guard that stops mattering, or a
# test that stops testing it, turns this red.
{ pkgs, studioDir }:

pkgs.runCommand "check-studio-api"
{
  nativeBuildInputs = [ pkgs.python3Minimal ];
  meta.description = "Kiln Studio's API refuses what it should, streams and cancels jobs, and every guard is proven";
}
  ''
    set -euo pipefail
    cp -r ${studioDir} studio
    chmod -R u+w studio
    mkdir -p "$out"
    python3 studio/tests/api_test.py | tee "$out/report.txt"
  ''
