# SPDX-License-Identifier: MIT
#
# nix/checks/studio-modules.nix — the studio's and the editors' ES modules load.
#
# tools/studio/tests/modules_check.mjs follows every page's entry script through
# its importmap, parses each module, checks each imported name is exported, and
# imports the modules that need no DOM. Then it is run on two broken copies of
# the tree — an import of a file that does not exist, and an import of a name
# that is not exported — and each must fail with its own message, so a resolver
# that quietly stopped following imports cannot pass.
{ pkgs, toolsDir }:

pkgs.runCommand "check-studio-modules"
{
  nativeBuildInputs = [ pkgs.nodejs ];
  meta.description = "every studio/editor ES module resolves, parses and exports what is imported from it";
}
  ''
    set -euo pipefail
    cp -r ${toolsDir} tools
    chmod -R u+w tools
    mkdir -p "$out"
    node tools/studio/tests/modules_check.mjs tools | tee "$out/report.txt"

    expect_fail() {  # label, pattern, dir
      if node "$3/studio/tests/modules_check.mjs" "$3" > "$out/$1.txt"; then
        echo "FAIL: $1 went unnoticed"; exit 1
      fi
      grep -q "$2" "$out/$1.txt" || { echo "FAIL: $1 failed for the wrong reason:"; cat "$out/$1.txt"; exit 1; }
      echo "  ok   $1 is caught"
    }

    cp -r tools missing-file
    sed -i 's#"webcommon/io.js"#"webcommon/no-such-file.js"#' missing-file/poser/src/main.js
    expect_fail missing-file "no-such-file.js does not exist" missing-file

    cp -r tools missing-name
    sed -i 's#import { createViewport }#import { createViewport, noSuchExport }#' missing-name/mapmaker/src/main.js
    expect_fail missing-name "imports noSuchExport" missing-name
  ''
