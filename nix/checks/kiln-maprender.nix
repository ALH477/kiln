# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-maprender.nix — the TOOL renders the same frame the GATE does.
#
# `./dev map-render` and nix/checks/kiln-map.nix compile the same
# tools/maprender/map_render.c and are held to the same
# nix/checks/refs/kiln-map.png. This check is what makes that a fact instead of
# a code-review convention: if either main() grows a kiln_gui_* or kiln_dd_*
# call of its own, the two frames diverge and this goes red.
#
# There is deliberately NO second reference image. A per-tool reference would
# only prove each tool agrees with itself, which is exactly the failure
# AGENTS.md names for per-architecture references — and exactly how
# tools/uipreview came to disagree with the console about kiln_gui for as long
# as it did.
{ pkgs, target, mapAsset, mapRenderSrc }:

target.mkCheck {
  pname = "maprender";
  sources = [ "${mapRenderSrc}/main.c" "${mapRenderSrc}/map_render.c" ];
  extraCFlags = [ "-I${mapRenderSrc}" ];
  args = "rom:/quake_test.map out.png";
  env = "KILN_HOST_DFS=fs";
  meta.description = "./dev map-render draws the frame kiln-map compares";
  preRun = ''
    mkdir -p fs && cp ${mapAsset} fs/quake_test.map
  '';
  script = ''
    if ! cmp -s out.png ${./refs/kiln-map.png}; then
      echo ""
      echo "FAILED: the tool and the gate no longer draw the same frame."
      echo "  reference $(stat -c%s ${./refs/kiln-map.png}) bytes, rendered $(stat -c%s out.png) bytes"
      echo "Either tools/maprender/main.c started drawing, or map_render.c"
      echo "changed and nix/checks/refs/kiln-map.png was not regenerated."
      exit 1
    fi
    echo "the tool renders the gate's reference frame (${target.description})"
  '';
}
