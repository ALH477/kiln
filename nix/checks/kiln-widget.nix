# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-widget.nix — the widget screens still render, and render the
# same.
#
# tools/uipreview draws kiln_widget's real screens on the host so a UI whose
# brief is "off-kilter" can actually be looked at. Nothing verified it, so it
# could rot silently — and it very nearly did something worse than rot: until
# it was moved onto plat/host it implemented kiln_gui's primitives ITSELF, and
# disagreed with the real ones about panel edge order, bar inset, and whether
# alpha blends at all. See tools/uipreview/uipreview.c's header.
#
# So this gate does two jobs. It keeps the preview buildable against the shared
# backend, and it holds the five committed screens byte-stable — which is what
# makes a design change show up as a reviewable diff rather than as a thing
# somebody notices later.
#
# kiln_widget is the only module in HOST_MODULES that compiles but has no
# assertions in kiln-logic (it calls into kiln_gui, so it cannot be linked
# standalone there). This is its coverage.
#
# ── One body, every architecture ───────────────────────────────────────
# Built by nix/host.nix's `target`, like every other host check. This one was
# the last to carry its own gcc line, which is fitting: it is the check that
# exists because tools/uipreview used to carry its own RASTERISER.
{ pkgs, target, uipreviewSrc }:

target.mkCheck {
  pname = "uipreview";
  sources = [ "${uipreviewSrc}/uipreview.c" ];
  args = "ui";
  meta.description = "kiln_widget's screens render byte-identically to their references";
  script = ''
    fail=0
    for s in title select results hud title-plain; do
      if ! cmp -s "ui-$s.png" "${uipreviewSrc}/ui-$s.png"; then
        echo "  FAILED: ui-$s.png differs from its reference"
        echo "    reference $(stat -c%s ${uipreviewSrc}/ui-$s.png) bytes, rendered $(stat -c%s ui-$s.png) bytes"
        fail=1
      fi
    done
    if [ $fail -ne 0 ]; then
      echo ""
      echo "If the change is intended, regenerate and LOOK at the result:"
      echo "  make -C tools/uipreview && tools/uipreview/uipreview tools/uipreview/ui"
      echo "A reference image is only as good as someone having looked at it."
      exit 1
    fi
    echo "all five widget screens match their references (${target.description})"
  '';
}
