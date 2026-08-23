# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-asset.nix — host round-trip of the kiln_asset layer against a
# database packed by the upstream C writer.
#
# kiln_asset.c is normally compiled into libkiln.a for the MIPS target, but the
# logic is host-portable: it calls streamdb_emb_* (which we build with the
# stdio backend here), sprite_load_buf (libdragon host build provides it), and
# t3d_model_load_buf (which we stub, since libt3d.a is MIPS-only — the stub
# records the buffer pointer so the check can verify kiln_asset_model routed
# the bytes correctly without actually parsing a T3D model).
#
# What this catches that the rom-streamdb-demo screenshot cannot:
#   * a regression in kiln_asset_load's key/buffer plumbing
#   * a regression in kiln_asset_find_suffix's passthrough
#   * kiln_asset_model / kiln_asset_sprite / kiln_asset_load returning the wrong
#     bytes or mis-sizing the malloc
# All on the host, in seconds, with a real packed DB.
{ pkgs, streamdbSrc, embeddedSrc, engineSrc }:

pkgs.runCommand "check-kiln-asset"
{
  nativeBuildInputs = [ pkgs.gcc pkgs.python3 ];
  meta.description = "kiln_asset runtime layer vs upstream-packed StreamDB";
}
  ''
    set -euo pipefail
    cp -r ${embeddedSrc} emb && chmod -R u+w emb
    cp -r ${engineSrc}/src/kiln/kiln_asset.c kiln_asset.c
    cp -r ${engineSrc}/src/kiln/kiln_asset.h kiln_asset.h

    # The engine's kiln_asset.h uses <streamdb/streamdb_embedded.h>, matching
    # the install layout (mips64-elf/include/streamdb/). The emb source tree
    # has the header at include/streamdb_embedded.h (no subdirectory). Provide
    # a matching layout so the engine source compiles unmodified.
    mkdir -p emb/include/streamdb
    cp ${embeddedSrc}/include/streamdb_embedded.h emb/include/streamdb/

    echo "── build the upstream C writer ──"
    gcc -O2 -std=gnu11 -I${streamdbSrc}/C/include -o pack \
        ${embeddedSrc}/test/pack.c \
        ${streamdbSrc}/C/src/streamdb.c -lpthread

    echo "── build the kiln_asset host test harness ──"
    # Copy the check driver next to kiln_asset.h so its `#include "kiln_asset.h"`
    # resolves in the build dir (a store-path source can't see build-dir
    # headers via the cwd).
    cp ${./kiln-asset-check.c} kiln-asset-check.c
    # The stub headers (libdragon.h, t3d/t3dmodel.h) live next to this file
    # and satisfy the engine's includes without pulling in libdragon/Tiny3D
    # (MIPS-only). The stubs record the bytes kiln_asset_model /
    # kiln_asset_sprite hand them so the check can verify the routing without
    # parsing a real T3D model or sprite.
    gcc -O2 -std=gnu11 -Wall -Wextra -Werror \
        -DSTREAMDB_EMB_BACKEND_STDIO=1 -DSTREAMDB_EMB_BACKEND_DFS=0 \
        -I. -I${./.} -I${./stub} -Iemb/include -I${engineSrc}/src \
        -o kilnasset_check \
        kiln-asset-check.c \
        kiln_asset.c \
        emb/src/streamdb_embedded.c emb/src/streamdb_io_stdio.c

    echo "── generate a corpus ──"
    python3 - <<'PY'
    import os, random
    random.seed(42)
    os.makedirs("corpus", exist_ok=True)
    pairs = []
    # One key per kiln_asset accessor the test exercises:
    #   models/cube.t3dm    -> kiln_asset_model (stub records bytes)
    #   sprites/logo.sprite -> kiln_asset_sprite (stub records bytes)
    #   levels/intro.bin    -> kiln_asset_load  (raw bytes)
    spec = [
        ("models/cube.t3dm",   b"T3M" + bytes([0x10]) + bytes(random.getrandbits(8) for _ in range(64))),
        ("sprites/logo.sprite", bytes(random.getrandbits(8) for _ in range(128))),
        ("levels/intro.bin",   b"KLNL" + bytes(random.getrandbits(8) for _ in range(20))),
    ]
    for i, (key, payload) in enumerate(spec):
        path = "corpus/f%d" % i
        with open(path, "wb") as f:
            f.write(payload)
        pairs.append((key, path))
    with open("args.txt", "w") as f:
        f.write(" ".join("%s %s" % p for p in pairs))
    PY

    ARGS="$(cat args.txt)"
    ./pack db.streamdb $ARGS > /dev/null

    echo "── run kiln_asset against the packed DB ──"
    ./kilnasset_check db.streamdb

    echo "kiln_asset check PASSED"
    mkdir -p $out && touch $out/ok
  ''