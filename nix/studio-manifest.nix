# SPDX-License-Identifier: MIT
#
# nix/studio-manifest.nix — lib.mkStudioManifest: the project model Kiln Studio
# reads, generated from a flake's own outputs rather than kept by hand.
#
# A flake's packages are a flat namespace — `fps`, `fps-combat`, `pc-fps`,
# `model-goblin`, `toolchain` — with nothing saying which is a ROM, which is a
# jump into one, which is its PC build and which is a tool. The builders know,
# so they say: mkN64Rom and hostNative/hostWasm.mkGame attach a `kiln` record
# (nix/rom.nix, nix/host.nix), and this function collects them into plain JSON
# grouped by game. Evaluation only — it reads passthru and never builds.
#
# Downstream games call the same function on their own packages, so the studio
# works in their repositories too:
#   studioManifest = kiln.lib.${system}.mkStudioManifest { inherit system; packages = self.packages.${system}; };
{ lib }:

{ system
, packages
, checks ? { }
, cheap ? [ ]
}:

let
  # `default` is an alias of another package; listing it would show every
  # game's main ROM twice.
  records = lib.filterAttrs (n: r: r != null && n != "default")
    (lib.mapAttrs (_: p: if builtins.isAttrs p then p.kiln or null else null) packages);

  # Derivation name -> package attribute, for the ROMs; a jump names its base by
  # derivation name, and a flake may expose `music` as `music-demo`.
  romAttr = lib.listToAttrs (lib.mapAttrsToList (attr: r: lib.nameValuePair r.name attr)
    (lib.filterAttrs (_: r: r.kind == "rom" && r ? name) records));
  resolved = lib.mapAttrs (_: r:
    if r.kind == "jump" then r // { base = romAttr.${r.base} or r.base; } else r) records;

  withName = lib.mapAttrsToList (name: r: r // { package = name; }) resolved;
  examples = lib.unique (map (r: r.example) withName);

  game = ex:
    let
      mine = builtins.filter (r: r.example == ex) withName;
      ofKind = k: builtins.filter (r: r.kind == k) mine;
      roms = ofKind "rom";
      hostBuilds = k: map (r: { inherit (r) package jump; }) (ofKind k);
    in
    {
      roms = map (r: r.package) roms;
      romTitle = if roms == [ ] then null else (builtins.head roms).romTitle;
      jumps = map (r: { inherit (r) package jump base; }) (ofKind "jump");
      pc = hostBuilds "pc";
      web = hostBuilds "web";
      # A game with no host build is one plat/host cannot run (skinned
      # characters, tile callbacks, vertex FX all abort there): console only.
      hostable = ofKind "pc" != [ ] || ofKind "web" != [ ];
    };
in
{
  version = 1;
  inherit system cheap;
  games = lib.genAttrs examples game;
  packages = resolved;
  tools = builtins.filter (n: !(records ? ${n}) && n != "default") (builtins.attrNames packages);
  checks = builtins.attrNames checks;
}
