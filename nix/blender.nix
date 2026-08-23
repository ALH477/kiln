# SPDX-License-Identifier: MIT
#
# nix/blender.nix — geometry authoring.
#
# This file owns the ENTIRE Blender strategy, the way nix/toolchain.nix owns
# the compiler: it is the only place that knows Blender exists, how to drive it
# without a display, and how its output becomes a .t3dm.
#
# ── Why Blender and not Fast64 ─────────────────────────────────────────
# Tiny3D's importer only reads Fast64 materials, and the obvious move is
# therefore to install Fast64. We do not, and the reason is worth writing down
# because it will otherwise get "fixed" later:
#
#   * Fast64 is not in nixpkgs. It would have to be a flake input pinned to a
#     GitHub revision, installed into Blender's addon path and enabled from
#     bpy at build time.
#   * Fast64 tracks Blender 4.2-4.5. This flake's nixpkgs ships Blender 5.2.
#     Every nixpkgs bump becomes a coin flip on whether the ROM still builds.
#   * None of that buys anything, because gltf_to_t3d does not talk to the
#     addon. It reads `materials[i].extras.f3d_mat` out of the glTF JSON —
#     which tools/f3d_inject.py writes directly, from a schema read straight
#     out of the importer's own parser.
#
# So Blender authors geometry (where it is genuinely the right tool) and
# f3d_inject authors materials (where the addon is just a JSON generator with
# a version-compatibility matrix attached).
#
# ── Headless ───────────────────────────────────────────────────────────
# `--background` needs no GPU and no display, unlike the emulator screenshots
# in tools/n64-shot.sh. Two flags are not optional:
#   --factory-startup   ignore user config AND drop the startup scene, which
#                       otherwise ships a default cube, camera and lamp into
#                       every export.
#   -noaudio            ONE dash. `--noaudio` is not an option; Blender treats
#                       it as a filename and tries to open it as a .blend.
# HOME must also be writable or Blender fails trying to create its config dir.
{ pkgs, n64Inst }:

let
  lib = pkgs.lib;
  blender = pkgs.blender;

  scripts = ../tools/blender;
  f3dInject = ../tools/f3d_inject.py;
in
rec {
  inherit blender;

  # One derivation for the whole chain: Blender -> f3d_inject -> gltf_to_t3d
  # -> mkasset. Splitting it would let the geometry cache separately from the
  # material, but a headless Blender run on models this size is a couple of
  # seconds, and a single derivation means the staging layout below exists in
  # exactly one place.
  #
  # Output follows nix/assets.nix's convention — $out/filesystem/<dest>/ — so
  # mkN64Rom's `assets` list consumes it with no special case.
  mkBlenderModel =
    { name
    , script # file in tools/blender/, e.g. "models.py"
    , model ? name # --model argument that script dispatches on
      # Overrides the `--model ${model}` argument entirely when set — for
      # scripts like quake_map.py/godot_scene.py that dispatch on an external
      # source file rather than a name in a hard-coded MODELS table. Kept
      # separate from `model` (rather than repurposing it) so the default
      # path stays exactly what it always was: no caller of the existing
      # `--model NAME` scripts has to change.
    , scriptArgs ? [ "--model" model ]
    , dest ? "models"
      # f3d_inject --material specs. "*=preset" covers every material at once.
      # See tools/f3d_inject.py for the preset list.
    , materials ? [ "*=shade" ]
      # A mkTextures derivation. Required if any material names a texture:
      # gltf_to_t3d decodes the PNG at conversion time to learn its dimensions,
      # so the image has to be present even though the ROM ships the .sprite.
    , textures ? null
    , bvh ? true
    , baseScale ? 64
      # Animated models carry a skin and one glTF animation per action.
    , animated ? false
      # Tiny3D's examples always follow gltf_to_t3d with `mkasset -c 2`, paired
      # with asset_init_compression(2) at runtime. -w 256 is the widest matching
      # window, which is right for assets loaded whole via asset_load() rather
      # than streamed with asset_fopen().
    , compress ? 2
    , window ? 256
    }:
    let
      materialArgs = lib.concatMapStringsSep " "
        (m: "--material ${lib.escapeShellArg m}") materials;
      scriptArgsStr = lib.concatMapStringsSep " " lib.escapeShellArg scriptArgs;
    in
    pkgs.stdenv.mkDerivation {
      pname = "model-${name}";
      version = "0.1.0";
      dontUnpack = true;

      nativeBuildInputs = [ blender pkgs.python3 n64Inst ];

      buildPhase = ''
        runHook preBuild
        export HOME="$TMPDIR"

        # ── staging layout ────────────────────────────────────────────
        # gltf_to_t3d rewrites a texture's on-disk path into the rom:/ path it
        # bakes into the .t3dm, and it does that by making the path relative to
        # the PROCESS CWD and replacing a leading --asset-path (main.cpp:34
        # sets projectPath = current_path(); materialParser.cpp mapRomPath does
        # the rewrite). Handed a bare Nix store path it never matches, and the
        # model ships with an absolute /nix/store texture path that cannot
        # resolve on a console. Hence: build inside a staging root laid out the
        # way the tool expects, and run it from there.
        #
        #   stage/assets/<name>.gltf         source, alongside...
        #   stage/assets/textures/*.png      ...its textures
        #   stage/filesystem/<dest>/         output -> rom:/<dest>/
        #
        # The output path must also contain the literal "filesystem/": streamed
        # animation sidecars get their rom:/ path by splitting on it
        # (writer.cpp getRomPath), so an animated model written anywhere else
        # loses its .sdata references.
        mkdir -p stage/assets/textures stage/filesystem/${dest} work

        ${lib.optionalString (textures != null)
          ''cp ${textures}/png/*.png stage/assets/textures/''}

        echo "── authoring ${name} (blender ${blender.version}) ──"
        blender --background --factory-startup -noaudio \
          --python ${scripts}/${script} \
          -- ${scriptArgsStr} --out "$PWD/work/${name}.gltf"

        # Blender writes <name>.gltf next to <name>.bin and refers to it by a
        # bare relative filename, so the pair has to move together.
        echo "── materials ──"
        python3 ${f3dInject} \
          work/${name}.gltf stage/assets/${name}.gltf ${materialArgs}
        cp work/${name}.bin stage/assets/${name}.bin

        echo "── convert ──"
        ( cd stage && gltf_to_t3d \
            assets/${name}.gltf filesystem/${dest}/${name}.t3dm \
            ${lib.optionalString bvh "--bvh"} \
            --base-scale=${toString baseScale} \
            --asset-path=assets/ \
            --verbose )

        # ── validate BEFORE compressing ───────────────────────────────
        # Both assertions below have to happen here rather than in checkPhase,
        # because mkasset wraps the file in a DCA container and neither the
        # magic nor the real size is visible afterwards.
        t3dm="stage/filesystem/${dest}/${name}.t3dm"

        # "T3M" + version byte; 0x04 for this Tiny3D pin (docs/modelFormat.md).
        magic=$(head -c 3 "$t3dm")
        if [ "$magic" != "T3M" ]; then
          echo "model '${name}': $t3dm starts with '$magic', not 'T3M' —" \
               "gltf_to_t3d produced something that is not a Tiny3D model" >&2
          exit 1
        fi

        # The signature failure of this pipeline: gltf_to_t3d SKIPS any
        # primitive whose material is missing or unnamed, silently and with a
        # zero exit status (parser.cpp:150-195). What you get is a structurally
        # valid, nearly empty .t3dm that draws nothing — otherwise discoverable
        # only by staring at a black screen much later.
        raw=$(stat -c%s "$t3dm")
        if [ "$raw" -lt 128 ]; then
          echo "model '${name}': $t3dm is only $raw bytes, which means" \
               "gltf_to_t3d skipped every mesh in it — check that each glTF" \
               "primitive has a NAMED material" >&2
          exit 1
        fi
        echo "  ${name}.t3dm: $raw bytes (uncompressed)"

        ${lib.optionalString animated ''
          # Streamed animations land beside the .t3dm as .sdata sidecars, and
          # their rom:/ paths are derived by splitting the output path on
          # "filesystem/" (writer.cpp getRomPath) — which is why the staging
          # layout above uses that exact directory name.
          echo "  streamed animation sidecars:" \
               "$(ls stage/filesystem/${dest}/*.sdata 2>/dev/null | wc -l)"
        ''}

        ${lib.optionalString (compress > 0) ''
          # In place: -o names a directory, and it is the file's own.
          ( cd stage && mkasset -c ${toString compress} -w ${toString window} \
              -o filesystem/${dest} filesystem/${dest}/${name}.t3dm )
        ''}

        runHook postBuild
      '';

      doCheck = true;
      checkPhase = ''
        runHook preCheck
        t3dm="stage/filesystem/${dest}/${name}.t3dm"
        [ -s "$t3dm" ] || { echo "model '${name}': no $t3dm produced" >&2; exit 1; }
        ${lib.optionalString (compress > 0) ''
          # libdragon's compressed-asset container (ASSET_MAGIC, asset_internal.h).
          # Its presence is also the reminder that the ROM must call
          # asset_init_compression(${toString compress}) before t3d_model_load.
          magic=$(head -c 3 "$t3dm")
          if [ "$magic" != "DCA" ]; then
            echo "model '${name}': expected a 'DCA' compressed asset after" \
                 "mkasset, got '$magic'" >&2
            exit 1
          fi
        ''}
        echo "  ${name}.t3dm: $(stat -c%s "$t3dm") bytes on ROM"
        runHook postCheck
      '';

      installPhase = ''
        runHook preInstall
        mkdir -p $out
        cp -r stage/filesystem $out/
        # The intermediate glTF is kept deliberately: when a model renders
        # wrong, the first question is always whether the geometry or the
        # material is at fault, and this is the artifact that answers it.
        mkdir -p $out/share/gltf
        cp stage/assets/${name}.gltf stage/assets/${name}.bin $out/share/gltf/
        runHook postInstall
      '';

      dontStrip = true;
      dontPatchELF = true;

      passthru = { modelName = "${name}.t3dm"; inherit dest; };

      meta.description = "Tiny3D model '${name}', authored in Blender";
    };

  # ── Quake .map / Godot .tscn import ─────────────────────────────────────
  # Once a level authored interactively (see tools/blender-mcp/) is finalised
  # and checked into the repo, it deserves the same hermetic, twice-built,
  # hash-compared treatment as any hand-authored model — these are thin
  # mkBlenderModel callers, not a second pipeline. See tools/blender/
  # quake_map.py and godot_scene.py for what each importer does and does not
  # support.
  mkQuakeMapModel =
    { name
    , src # the .map file
    , scale ? null # quake_map.py's Quake-units -> Blender-units factor; null = script default (1/32)
    , ...
    }@args:
    mkBlenderModel ((builtins.removeAttrs args [ "src" "scale" ]) // {
      script = "quake_map.py";
      scriptArgs = [ "--map" "${src}" ]
        ++ lib.optionals (scale != null) [ "--scale" (toString scale) ];
    });

  # `src` is the Godot PROJECT ROOT (a directory), not just the .tscn — a
  # scene's res:// mesh references are resolved against it, so the whole
  # subtree of referenced .glb/.gltf/.obj files has to be part of the Nix
  # input, not just the scene file naming them.
  mkGodotSceneModel =
    { name
    , src # the Godot project root directory
    , scenePath # path to the .tscn, relative to `src`
    , scale ? null
    , ...
    }@args:
    mkBlenderModel ((builtins.removeAttrs args [ "src" "scenePath" "scale" ]) // {
      script = "godot_scene.py";
      scriptArgs = [ "--scene" "${src}/${scenePath}" "--project" "${src}" ]
        ++ lib.optionals (scale != null) [ "--scale" (toString scale) ];
    });

  # ── Morph targets ───────────────────────────────────────────────────────
  # gltf_to_t3d does not parse glTF morph targets, so the engine's kiln_morph
  # module blends between sibling .t3dm models at runtime. This builder is a
  # thin wrapper over mkBlenderModel that documents the convention: the Blender
  # script must create N mesh objects with identical topology (same vertex and
  # face count, same order) named `<name>_base`, `<name>_tall`, `<name>_wide`,
  # etc. The engine loads each as a separate .t3dm and extracts their vertex
  # buffers with t3d_model_get_vertices.
  #
  # The build itself is unchanged — gltf_to_t3d exports all objects in the
  # scene into one .t3dm. The runtime side calls t3d_model_load per target
  # .t3dm (or loads them all and indexes by name).
  mkMorphModel =
    { name
    , script
    , model ? name
    , ...
    }@args:
    mkBlenderModel (args // {
      inherit name script model;
      # BVH is pointless for morph targets — the vertex buffer is what matters,
      # not the collision/bvh structure. Disabling saves a few hundred bytes.
      bvh = args.bvh or false;
    });
}
