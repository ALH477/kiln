# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-names.nix — the old name stays gone.
#
# The engine was called M64 until it was renamed to Kiln, for reasons that
# were not cosmetic: "M64" is ModRetro's shipping console, and "m64p_"/".m64"
# is mupen64plus' plugin API and movie format. Both collisions are worse on
# the PC target than on the console one.
#
# A rename is not a state, it is an invariant, and this is the only kind of
# gate that can hold it. 9,393 occurrences across 330 files were swept in one
# pass; what makes that stick is not the sweep but the fact that reintroducing
# the name now fails the build. Without this check the tree drifts back one
# comment at a time — the same failure an X-macro table (name -> enum/path/
# label generated from one list) exists to prevent for a name to enum/path
# mapping, applied here to a single name instead.
#
# HOW: each line has every allowlisted phrase struck out of it before it is
# searched. Striking phrases rather than skipping whole lines is what lets
# `xm64player_stop(&g_music)` pass while a line mentioning both xm64player AND
# a stray m64_ still fails.
#
# What is searched for depends on the file, because two different things are
# spelled the same way:
#
#   IDENTIFIER forms — m64_, M64_, libm64, -lm64, M64<uppercase>, m64.mk,
#   src/m64, and friends — name something that no longer exists. They fail
#   EVERYWHERE, prose included, because a dangling identifier in a document is
#   just a document that lies.
#
#   The bare WORD "M64" is allowed in .md and .txt. In prose it is either the
#   ModRetro console or the project's own history, and this file's whole
#   subject is that the two used to be confused; a gate that could not let
#   CLAUDE.md explain the rename would be a gate nobody could document.
#   Everywhere else — code, Nix, Makefiles, shell — the bare word still fails,
#   since there it is a string literal, a path or a title.
#
# The allowlist is nix/checks/kiln-names-allow.txt and it is the whole
# definition of what "M64" is still allowed to mean. See its header.
{ pkgs, repo }:

pkgs.runCommand "check-kiln-names"
{
  nativeBuildInputs = [ pkgs.perl ];
  meta.description = "the M64 name does not come back";
}
  ''
    set -euo pipefail

    perl -e '
      my ($allow, $root) = (shift, shift);
      open my $a, "<", $allow or die "allowlist: $!";
      my @p;
      while (<$a>) { chomp; next if /^\s*#/ || /^\s*$/; push @p, $_; }
      close $a;
      die "allowlist is empty\n" unless @p;

      my ($bad, $scanned) = (0, 0);
      my @stack = ($root);
      while (my $d = pop @stack) {
        opendir my $dh, $d or next;
        for my $e (readdir $dh) {
          next if $e eq "." || $e eq "..";
          my $f = "$d/$e";
          if (-d $f) { push @stack, $f; next; }
          next unless -f $f;
          # This check and its allowlist necessarily spell out the name they
          # ban. A linter is not a violation of its own rule.
          next if $e =~ /^kiln-names(\.nix|-allow\.txt)$/;
          open my $fh, "<:raw", $f or next;
          my $head = ""; read $fh, $head, 4096;
          next if $head =~ /\0/;          # binary
          seek $fh, 0, 0;
          $scanned++;
          my $rel = $f; $rel =~ s/^\Q$root\E\/?//;
          my $is_doc = ($e =~ /\.(md|txt)$/);
          my $n = 0;
          while (my $l = <$fh>) {
            $n++;
            my $s = $l;
            for my $p (@p) { $s =~ s/\Q$p\E//g; }
            my $hit = ($s =~ m{m64_|M64_|libm64|-lm64|m64\.mk|m64-inst
                              |M64[A-Z]|m64lib|m64jingle|m64logic|m64asset
                              |m64Logo|m64Jingle|src/m64|include/m64}x)
                   || (!$is_doc && $s =~ /[mM]64/);
            next unless $hit;
            chomp $l;
            print "  $rel:$n: $l\n";
            $bad++;
          }
          close $fh;
        }
        closedir $dh;
      }
      die "scanned no files - the check is not looking at the tree\n"
        unless $scanned > 50;
      if ($bad) {
        print "\nFAILED: $bad line(s) still name the engine M64.\n";
        print "If an occurrence is the ModRetro console or a libdragon/Fast64\n";
        print "identifier, add the phrase to nix/checks/kiln-names-allow.txt\n";
        print "and say which group it is in. Otherwise rename it to kiln.\n";
        exit 1;
      }
      print "no residual M64 outside the allowlist ($scanned files scanned)\n";
    ' ${./kiln-names-allow.txt} ${repo}

    mkdir -p $out && touch $out/ok
  ''
