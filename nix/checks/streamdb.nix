# SPDX-License-Identifier: MIT
#
# nix/checks/streamdb.nix — round-trip the embedded reader against databases
# written by the UPSTREAM C library.
#
# This is the highest-value check in the repo per line: the reader parses a
# third-party binary format, and the N64 has no way to tell you why a parse
# failed. Catching it on the host, in the sandbox, with the real writer, is the
# only sane place. It already earned its keep — it caught that the index's
# `offset` field addresses the payload rather than the record header, which the
# upstream format comment does not state and which fails on every document.
{ pkgs, streamdbSrc, embeddedSrc }:

pkgs.runCommand "check-streamdb"
{
  nativeBuildInputs = [ pkgs.gcc pkgs.python3 ];
  meta.description = "StreamDB embedded reader vs upstream writer";
}
  ''
    set -euo pipefail
    cp -r ${embeddedSrc} emb && chmod -R u+w emb

    echo "── build the upstream C writer ──"
    gcc -O2 -std=gnu11 -I${streamdbSrc}/C/include -o pack \
        ${./../../streamdb-embedded/test/pack.c} \
        ${streamdbSrc}/C/src/streamdb.c -lpthread

    echo "── build the embedded reader (host backend) ──"
    gcc -O2 -std=gnu11 -Wall -Wextra -Werror -Iemb/include -o roundtrip \
        ${./../../streamdb-embedded/test/roundtrip.c} \
        emb/src/streamdb_embedded.c emb/src/streamdb_io_stdio.c

    echo "── generate a corpus ──"
    python3 - <<'PY'
    import os, random
    random.seed(1234)
    os.makedirs("corpus", exist_ok=True)
    kinds = [("models","t3dm"),("sprites","sprite"),("sfx","wav64"),
             ("music","xm64"),("data","bin")]
    pairs = []
    for i in range(120):
        d, e = kinds[i % len(kinds)]
        key = "%s/asset_%03d.%s" % (d, i, e)
        path = "corpus/f%03d" % i
        with open(path, "wb") as f:
            f.write(bytes(random.getrandbits(8) for _ in range(random.randint(1, 3000))))
        pairs.append((key, path))
    with open("args.txt", "w") as f:
        f.write(" ".join("%s %s" % p for p in pairs))
    PY

    ARGS="$(cat args.txt)"
    ./pack db.streamdb $ARGS > /dev/null
    ./roundtrip db.streamdb $ARGS

    # The two negative cases below are EXPECTED to fail, so their output is
    # captured rather than printed — otherwise a passing check is full of
    # "FAIL:" lines and nobody can read it.
    echo "── a truncated database must be rejected, not crash ──"
    head -c 300 db.streamdb > trunc.streamdb
    if ./roundtrip trunc.streamdb > trunc.log 2>&1; then
      echo "FAIL: truncated database was accepted" >&2; cat trunc.log >&2; exit 1
    fi
    grep -q 'bad format' trunc.log \
      || { echo "FAIL: expected a format error, got:" >&2; cat trunc.log >&2; exit 1; }
    echo "  rejected with: $(grep -m1 'FAIL' trunc.log | sed 's/^ *FAIL: *//')"

    echo "── one flipped payload byte must fail its CRC ──"
    cp db.streamdb bad.streamdb
    python3 -c "
    f=open('bad.streamdb','r+b'); f.seek(400); b=f.read(1)
    f.seek(400); f.write(bytes([b[0]^0xFF])); f.close()"
    if ./roundtrip bad.streamdb $ARGS > bad.log 2>&1; then
      echo "FAIL: corrupted payload was accepted" >&2; exit 1
    fi
    grep -q 'checksum mismatch' bad.log \
      || { echo "FAIL: expected a CRC error, got:" >&2; tail -5 bad.log >&2; exit 1; }
    # Exactly one document was corrupted, so exactly one must fail.
    n=$(grep -c 'checksum mismatch' bad.log)
    [ "$n" = 1 ] || { echo "FAIL: $n CRC errors, expected 1" >&2; exit 1; }
    echo "  rejected with: checksum mismatch (1 document, as corrupted)"

    echo "streamdb check PASSED"
    mkdir -p $out && touch $out/ok
  ''
