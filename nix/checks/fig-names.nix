# SPDX-License-Identifier: MIT
#
# nix/checks/fig-names.nix — the engine's prefix does not drift back.
#
# This is nix/checks/kiln-names.nix's argument applied a second time, and that
# file states it better than a restatement would:
#
#   "A rename is not a state, it is an invariant, and this is the only kind of
#   gate that can hold it. 9,393 occurrences across 330 files were swept in one
#   pass; what makes that stick is not the sweep but the fact that
#   reintroducing the name now fails the build. Without this check the tree
#   drifts back one comment at a time."
#
# That rename was the pre-Kiln engine name; see kiln-names.nix, which is
# allowed to spell it. This one is the engine's half of Kiln -> Figulina
# (docs/NAMING.md), and it is HARDER to gate, because unlike that one the
# word "Kiln" is still correct — it is the forge, the flake, `./dev`, and
# `lib.mkN64Rom`. So this gate cannot search for a word. It searches for the
# IDENTIFIER forms, and only where they are now wrong.
#
# ── Where it looks ─────────────────────────────────────────────────────
# engine/src and plat only. Those are Figulina (section 4). Everything else in
# this repo is Kiln the forge, where `kiln` is the right answer and a gate
# that said otherwise would be the layer violation section 10 forbids.
#
# ── What is allowed to survive there, and why ──────────────────────────
#  - kiln_<module>.h / .c — FILE names. Section 9 step 2 moves symbols, not
#    files; step 7 considers the layout. Until then `#include <kiln_skel.h>`,
#    a forge script like tools/blender/kiln_logo.py, and a forge ASSET like
#    dsp/kiln_jingle.dsp or assets/kiln_body.glb — the splash renders a kiln
#    because the kiln is the forge (section 8), so those keep their name.
#    is correct and this gate must not say otherwise.
#  - kiln_compat.h and the defines inside it. That file IS the old prefix, on
#    purpose, for one train (step 2). It is skipped whole.
#  - KILN_DEBUG, KILN_JUMP, KILN_HOST_DFS, KILN_SRC — the forge's build knobs,
#    set by Nix and make and read by C. They cross the seam in the direction
#    section 4 allows, and renaming them would have broken a cross-repo build
#    flag inside a 11,442-occurrence sweep.
#
# Everything else — a new kiln_foo() in engine/src, a KilnBar typedef, a
# KILN_BAZ macro — fails here, which is the only thing that keeps the
# compatibility train finite.
{ pkgs, repo }:

pkgs.runCommand "check-fig-names"
{
  nativeBuildInputs = [ pkgs.perl ];
  meta.description = "the engine's symbols stay fig_, not kiln_";
}
  ''
    set -euo pipefail

    perl -e '
      my $root = shift;
      my @allow_macro = qw(KILN_DEBUG KILN_JUMP KILN_HOST_DFS KILN_SRC);
      my %allow = map { $_ => 1 } @allow_macro;

      my ($bad, $scanned) = (0, 0);
      my @stack = ("$root/engine/src", "$root/plat");
      while (my $d = pop @stack) {
        opendir(my $dh, $d) or next;
        for my $e (readdir $dh) {
          next if $e eq "." || $e eq "..";
          my $p = "$d/$e";
          if (-d $p) { push @stack, $p; next; }
          next unless $e =~ /\.(c|h)$/;
          # The shim is the one file allowed to spell the old prefix.
          next if $e eq "kiln_compat.h";
          $scanned++;
          open(my $fh, "<", $p) or next;
          my $ln = 0;
          while (my $l = <$fh>) {
            $ln++;
            my $s = $l;
            # Strike out FILE references before searching. A path or an
            # #include naming kiln_skel.h is correct until section 9 step 7.
            $s =~ s/\bkiln_[A-Za-z0-9_]+\.(h|c|o|py|mk|dsp|gltf|glb)\b//g;
            # Strike the allowed forge knobs.
            $s =~ s/\b$_\b//g for @allow_macro;
            if ($s =~ /\b(kiln_[A-Za-z0-9_]+|KILN_[A-Za-z0-9_]+|Kiln[A-Z][A-Za-z0-9_]*)\b/) {
              my $hit = $1;
              print STDERR "$p:$ln: $hit\n";
              print STDERR "    $l";
              $bad++;
            }
          }
          close $fh;
        }
        closedir $dh;
      }
      if ($bad) {
        print STDERR "\n$bad occurrence(s) of the old engine prefix.\n";
        print STDERR "docs/NAMING.md section 4: Figulina C is fig_. The engine\n";
        print STDERR "symbol is fig_<object>_<verb>; the type is Fig<Noun>; the\n";
        print STDERR "macro is FIG_<NAME>. `kiln` is still right for the FORGE —\n";
        print STDERR "the flake, ./dev, lib.mkN64Rom — and for these file names\n";
        print STDERR "until section 9 step 7 moves them.\n";
        print STDERR "\nIf you are ADDING a bridge for a downstream, it goes in\n";
        print STDERR "engine/src/kiln/kiln_compat.h, which is skipped here.\n";
        exit 1;
      }
      print "the engine prefix holds: $scanned sources, no kiln_ symbols\n";
    ' ${repo}

    touch $out
  ''
