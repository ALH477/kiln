# SPDX-License-Identifier: MPL-2.0
#
# nix/checks/rom.nix — validate a built .z64 without hardware or an emulator.
#
# A ROM that is subtly malformed boots to a black screen and tells you nothing.
# These are the cheap structural facts that can be asserted offline, so that
# "it built" means a bit more than "make exited 0".
{ pkgs, rom, name ? rom.pname or "rom" }:

pkgs.runCommand "check-rom-${name}"
{
  meta.description = "structural validation of ${name}.z64";
}
  ''
    set -euo pipefail
    z64="$(find ${rom} -maxdepth 1 -name '*.z64' | head -1)"
    [ -n "$z64" ] || { echo "no .z64 in ${rom}" >&2; exit 1; }
    echo "checking $z64"

    # 1. Magic. 0x80371240 is the big-endian (.z64 / "native") byte order.
    #    0x37804012 would be .v64 (byte-swapped) and 0x40123780 .n64 (word-
    #    swapped) — both boot on nothing without conversion, and are the
    #    classic way a homebrew ROM ends up mysteriously dead.
    magic=$(od -An -tx1 -N4 "$z64" | tr -d ' \n')
    if [ "$magic" != "80371240" ]; then
      echo "FAIL: bad magic $magic (expected 80371240 = big-endian .z64)" >&2
      case "$magic" in
        37804012) echo "      this is .v64 byte-swapped" >&2 ;;
        40123780) echo "      this is .n64 word-swapped" >&2 ;;
      esac
      exit 1
    fi
    echo "  magic: .z64 big-endian OK"

    # 2. Minimum size. Real carts and most flashcarts expect at least 1 MiB;
    #    n64tool pads to this, so a smaller file means padding was skipped.
    size=$(stat -c%s "$z64")
    echo "  size: $size bytes"
    if [ "$size" -lt 1048576 ]; then
      echo "  NOTE: under 1 MiB — fine for emulators and SC64, but some" >&2
      echo "        flashcarts and real carts expect >= 1 MiB." >&2
    fi

    # 3. The ROM header title lives at 0x20 and is 20 bytes of ASCII.
    title=$(dd if="$z64" bs=1 skip=32 count=20 2>/dev/null | tr -d '\0')
    echo "  title: '$title'"
    if [ -z "$title" ]; then
      echo "FAIL: empty ROM title — N64_ROM_TITLE did not reach n64tool" >&2
      exit 1
    fi
    # Non-ASCII in the header is not rejected by n64tool but displays as
    # garbage in flashcart menus and ROM databases.
    if printf '%s' "$title" | LC_ALL=C grep -q '[^ -~]'; then
      echo "FAIL: ROM title contains non-ASCII bytes" >&2
      exit 1
    fi

    echo "rom check PASSED"
    mkdir -p $out
    echo "$title" > $out/title
  ''
