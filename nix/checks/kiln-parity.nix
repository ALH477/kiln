# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-parity.nix — engine/modules.mk's HOST_MODULES tells the truth.
#
# The engine is being made to build for a second target, and the failure mode
# that matters is not "the host build breaks" — that is loud. It is the tier
# list quietly disagreeing with reality: a module listed as host-clean that has
# not compiled in months, or a module that became host-clean and nobody
# noticed, so it never gets the -Werror second opinion or the sanitizers.
#
# So this gate checks the list in BOTH directions from one run:
#
#   every name in HOST_MODULES        must compile natively at -Werror
#   every name absent from it         must NOT compile natively
#
# which makes it self-verifying: there is no way to make it pass by weakening
# it. Deleting a name fails (that module still compiles). Adding a name fails
# (that module still does not). It is the only shape of gate that can hold a
# claim about a partition.
#
# ── The -Werror is not incidental ─────────────────────────────────────
# engine/Makefile sets -Wno-error deliberately, for third-party header noise
# on the cross build. Compiling the same sources natively at -Werror is a free
# second opinion, and on its first run over the widened tier it found a dead
# bounds check in kiln_surface.c: `assertf(id < KILN_SURFACE_MAX)` where id is
# a uint8_t and the max is 256, so the guard could never fire. That warning had
# been reported to nobody for as long as the module existed. See the
# _Static_assert that replaced it.
{ pkgs, engineSrc, platHost, hostMath, streamdbInc }:

pkgs.runCommand "check-kiln-parity"
{
  nativeBuildInputs = [ pkgs.gcc pkgs.gnumake ];
  meta.description = "HOST_MODULES is exactly the set that compiles natively";
}
  ''
    set -euo pipefail

    cp -r ${engineSrc} engine && chmod -R u+w engine
    ALL=$(make -s -C engine -f modules.mk print-modules)
    HOST=$(make -s -C engine -f modules.mk print-host-modules)

    echo "modules: $(echo $ALL | wc -w), claimed host-clean: $(echo $HOST | wc -w)"

    CFLAGS="-c -O1 -std=gnu2x -Wall -Wextra -Werror"
    INCS="-I${platHost}/include -I${hostMath}/include -I${streamdbInc} \
           -Iengine/src -Iengine/src/kiln -DSTREAMDB_EMB_BACKEND_DFS=1"

    bad=0
    for m in $ALL; do
      claimed=no
      for h in $HOST; do [ "$h" = "$m" ] && claimed=yes; done

      if gcc $CFLAGS $INCS -o "$m.o" "engine/src/kiln/$m.c" 2>"$m.log"; then
        got=yes
      else
        got=no
      fi

      if [ "$claimed" != "$got" ]; then
        bad=$((bad + 1))
        if [ "$claimed" = yes ]; then
          echo "  FAIL $m: listed in HOST_MODULES but does not compile natively:"
          sed 's/^/      /' "$m.log" | head -8
        else
          echo "  FAIL $m: compiles natively but is NOT in HOST_MODULES."
          echo "      Add it, so it gets -Werror and the sanitizers."
        fi
      fi
    done

    if [ $bad -gt 0 ]; then
      echo ""
      echo "FAILED: $bad module(s) disagree with engine/modules.mk's HOST_MODULES."
      exit 1
    fi

    echo "HOST_MODULES is exact: every listed module compiles, no unlisted one does"
    mkdir -p $out && touch $out/ok
  ''
