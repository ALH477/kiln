# SPDX-License-Identifier: MIT
#
# interceptor-demo: jump ROMs and host builds. See nix/demos/README.md.
#
# "Kiln Interceptor" is 16 of the header's 20 characters, so a jump name gets
# three: TTL (credit title, held), ORB / FIG / CIN (one showcase mode, latched)
# and FLY (free flight on a steering tape).
ctx: with ctx;
let
  a = args.interceptorDemoArgs;
  jumps = [ "TTL" "ORB" "FIG" "CIN" "FLY" ];
  pc = jump: {
    name = "pc-interceptor-demo${lib.optionalString (jump != null) "-${lib.toLower jump}"}";
    value = hostNative.mkGame {
      pname = "kiln-interceptor-demo${lib.optionalString (jump != null) "-${lib.toLower jump}"}";
      sources = [ ../../examples/interceptor-demo/main.c ];
      assets = a.assets;
      extraCFlags = lib.optional (jump != null) "-DKILN_JUMP=JUMP_${jump}";
      meta.description = "interceptor-demo${lib.optionalString (jump != null) " (${jump})"}, on this machine";
    };
  };
in
mkJumpRoms a jumps
// lib.listToAttrs (map pc ([ null ] ++ jumps))
