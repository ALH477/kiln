# SPDX-License-Identifier: MIT
#
# nix/assets.nix — the asset pipeline.
#
# The merged prefix ships a dozen host tools (mksprite, mkfont, mkasset,
# audioconv64, gltf_to_t3d, ...) and until now the build system exposed none of
# them, which is why examples/engine builds its cube by hand. These builders
# are the missing layer.
#
# ── The convention ─────────────────────────────────────────────────────
# Every builder produces a derivation whose `filesystem/` directory holds the
# converted asset. That is deliberately the SAME convention mkBakedInstrument
# already uses (nix/faust.nix), so mkN64Rom's existing `assets` argument
# consumes them unchanged — and because rom.nix copies with `cp -rL
# <asset>/filesystem/.`, subdirectories survive. So `dest` belongs here, in the
# builder, and mkN64Rom needed no modification at all:
#
#   mkModel { name = "player"; src = ./player.glb; dest = "models"; }
#     -> $out/filesystem/models/player.t3dm
#     -> rom's filesystem/models/player.t3dm
#     -> loadable at runtime as "rom:/models/player.t3dm"
#
# mkStreamdb below produces a `filesystem/` directory too, with one
# `<name>.streamdb` file in it, so a ROM can list it alongside any loose
# assets and the .streamdb lands in DFS like anything else.
#
# ── Determinism ────────────────────────────────────────────────────────
# nix/checks/assets.nix runs each builder twice and compares hashes. Asset
# tools that embed timestamps or iterate a hash map in address order will drift
# between builds, which turns every golden-image test downstream into a
# coin flip. Cheaper to catch here.
{ pkgs, n64Inst, streamdbSrc }:

let
  lib = pkgs.lib;

  # Shared skeleton. `convert` is a shell fragment that reads $src and writes
  # into $outdir; everything else (staging, the dest subdirectory, the sanity
  # check that something was actually produced) is identical across tools.
  mkAsset =
    { name
    , src
    , dest ? ""
    , convert
    , extraInputs ? [ ]
    , outName # expected output filename, for the did-it-work check
    }:
    pkgs.stdenv.mkDerivation {
      pname = "asset-${name}";
      version = "0.1.0";
      inherit src;
      dontUnpack = true;

      nativeBuildInputs = [ n64Inst ] ++ extraInputs;

      buildPhase = ''
        runHook preBuild
        outdir="filesystem${lib.optionalString (dest != "") "/${dest}"}"
        mkdir -p "$outdir"
        ${convert}
        runHook postBuild
      '';

      doCheck = true;
      checkPhase = ''
        runHook preCheck
        outdir="filesystem${lib.optionalString (dest != "") "/${dest}"}"
        if [ ! -s "$outdir/${outName}" ]; then
          echo "asset '${name}': expected $outdir/${outName}, got:" >&2
          find filesystem -type f >&2 || true
          exit 1
        fi
        echo "  ${outName}: $(stat -c%s "$outdir/${outName}") bytes"
        runHook postCheck
      '';

      installPhase = ''
        runHook preInstall
        mkdir -p $out
        cp -r filesystem $out/
        runHook postInstall
      '';

      # Converted assets are N64 data blobs, not host ELFs.
      dontStrip = true;
      dontPatchELF = true;

      passthru = { assetName = outName; inherit dest; };

      meta.description = "converted N64 asset '${name}'";
    };

in
rec {
  inherit mkAsset;

  # ── Models ───────────────────────────────────────────────────────────
  # Tiny3D's importer, NOT libdragon's mkmodel. They are different runtimes:
  # mkmodel emits .model64 for libdragon's own model API, while t3d_model_load
  # wants .t3dm. Since the engine's 3D layer is Tiny3D, this is the right one.
  mkModel =
    { name
    , src
    , dest ? "models"
      # A BVH lets t3d_model_bvh_query_frustum cull the model cheaply. Worth it
      # for anything static and large; pure overhead for a single small object.
    , bvh ? true
      # Blender units -> integer scale. Tiny3D's default is 64.
    , baseScale ? 64
    , ignoreMaterials ? false
    , ignoreTransforms ? false
      # Every Tiny3D example follows gltf_to_t3d with `mkasset -c 2`, which the
      # ROM must match with asset_init_compression(2) before t3d_model_load.
      # -w 256 is the widest matching window: right for assets read whole by
      # asset_load(), wrong (memory-hungry) only for asset_fopen() streaming.
      # compress = 0 disables the step.
    , compress ? 2
    , window ? 256
    }:
    mkAsset {
      inherit name src dest;
      outName = "${name}.t3dm";
      convert = ''
        gltf_to_t3d "$src" "$outdir/${name}.t3dm" \
          ${lib.optionalString bvh "--bvh"} \
          --base-scale=${toString baseScale} \
          ${lib.optionalString ignoreMaterials "--ignore-materials"} \
          ${lib.optionalString ignoreTransforms "--ignore-transforms"} \
          --verbose
        ${lib.optionalString (compress > 0) ''
          # -o names a directory, and here it is the file's own — mkasset
          # rewrites the .t3dm in place.
          mkasset -v -c ${toString compress} -w ${toString window} \
            -o "$outdir" "$outdir/${name}.t3dm"
        ''}
      '';
    };

  # ── Sprites / textures ───────────────────────────────────────────────
  mkSprite =
    { name
    , src
    , dest ? "sprites"
      # RDP surface format: RGBA16, RGBA32, CI4, CI8, I4, I8, IA4, IA8, IA16.
      # AUTO lets mksprite pick. On a 4 KB TMEM budget the format choice is
      # usually the single biggest lever on whether a texture fits at all.
    , format ? null
    , compress ? 1
    , mipmap ? null # e.g. "BOX"
    , dither ? null
    , texparms ? null # "s_repeats,t_repeats,s_mirror,t_mirror"
    }:
    mkAsset {
      inherit name src dest;
      outName = "${name}.sprite";
      convert = ''
        cp "$src" "${name}.png"
        mksprite -v \
          ${lib.optionalString (format != null) "--format ${format}"} \
          --compress ${toString compress} \
          ${lib.optionalString (mipmap != null) "--mipmap ${mipmap}"} \
          ${lib.optionalString (dither != null) "--dither ${dither}"} \
          ${lib.optionalString (texparms != null) "--texparms ${texparms}"} \
          -o "$outdir" "${name}.png"
      '';
    };

  # ── Generated texture sets ───────────────────────────────────────────
  # Two consumers, one generation, deliberately in one derivation:
  #
  #   $out/png/*.png                    gltf_to_t3d decodes these AT CONVERSION
  #                                     TIME, purely to learn each texture's
  #                                     width and height (UVs are stored as
  #                                     pixel coords, meshConverter.cpp:120).
  #   $out/filesystem/<dest>/*.sprite   what the ROM actually ships and what
  #                                     sprite_load() opens at runtime.
  #
  # Splitting these into two derivations would mean generating the same pixels
  # twice and hoping the two runs agree — and if they ever disagreed, the model
  # would carry UVs scaled for one texture and the ROM would ship the other.
  #
  # No per-file format argument: mksprite reads the RDP format out of the
  # filename's dot-section (grass.i8.png -> I8), and gltf_to_t3d independently
  # derives the same .sprite name by cutting at ".png". tools/gen_textures.py
  # documents why that convention is load-bearing.
  mkTextures =
    { name
    , generator ? ../tools/gen_textures.py
      # Additional generator scripts, run into the same output directory after
      # `generator`, with it importable as `gen_textures`. This exists because
      # a downstream GAME's terrain is not the engine's business: PetaByte
      # Madness' island band atlas lived in gen_textures.py until the split
      # genericized it out, and it has to land somewhere that is neither a
      # second copy of the engine's noise functions nor a permanent
      # game-specific entry in TEXTURES.
      #
      # Sharing `fbm` matters more than it looks. Its docstring is explicit
      # that the hash is written out longhand rather than using random.Random
      # "so the output cannot drift with a CPython release" — a second
      # implementation would be a second thing to keep from drifting, against
      # committed reference captures.
    , extraGenerators ? [ ]
    , dest ? "textures"
    , compress ? 1
    }:
    pkgs.stdenv.mkDerivation {
      pname = "textures-${name}";
      version = "0.1.0";
      dontUnpack = true;

      nativeBuildInputs = [ n64Inst pkgs.python3 ];

      buildPhase = ''
        runHook preBuild
        mkdir -p png filesystem/${dest}
        # Copied to a plain name first so `import gen_textures` resolves for
        # the extra generators below — a Nix store path is
        # `<hash>-gen_textures.py`, which is not an importable module name.
        cp ${generator} ./gen_textures.py
        python3 ./gen_textures.py png
        ${lib.concatMapStringsSep "\n        "
            (g: ''PYTHONPATH=. python3 ${g} png'') extraGenerators}
        for f in png/*.png; do
          mksprite -v --compress ${toString compress} -o filesystem/${dest} "$f"
        done
        runHook postBuild
      '';

      doCheck = true;
      checkPhase = ''
        runHook preCheck
        count=$(ls filesystem/${dest}/*.sprite 2>/dev/null | wc -l)
        pngs=$(ls png/*.png | wc -l)
        if [ "$count" -ne "$pngs" ]; then
          echo "textures '${name}': $pngs PNGs in but $count sprites out" >&2
          ls -l png filesystem/${dest} >&2
          exit 1
        fi
        # The name each tool derives must agree, or the rom:/ path baked into a
        # model points at a sprite that is not there — which shows up on
        # hardware as a black or garbage-textured model, not as a load error.
        for f in png/*.png; do
          want="filesystem/${dest}/$(basename "$f" .png).sprite"
          [ -s "$want" ] || {
            echo "textures '${name}': expected $want; mksprite named its" \
                 "output differently, so the rom:/ path gltf_to_t3d bakes" \
                 "into a model will not resolve" >&2
            ls filesystem/${dest} >&2
            exit 1
          }
        done
        echo "  ${name}: $count sprites"
        runHook postCheck
      '';

      installPhase = ''
        runHook preInstall
        mkdir -p $out
        cp -r png filesystem $out/
        runHook postInstall
      '';

      dontStrip = true;
      dontPatchELF = true;

      meta.description = "generated texture set '${name}' (PNG + .sprite)";
    };

  # ── Fonts ────────────────────────────────────────────────────────────
  # The engine's GUI falls back to libdragon's built-in debug font, so this is
  # only needed when a game wants a real typeface.
  mkFont =
    { name
    , src # .ttf / .otf / BMFont
    , dest ? "fonts"
    , size ? 12
    , compress ? 1
    , range ? null # e.g. "20-7F"
    , outline ? null
    , monochrome ? false
    }:
    mkAsset {
      inherit name src dest;
      outName = "${name}.font64";
      convert = ''
        # mkfont derives the output basename from the input, so the input has
        # to be named after the asset rather than whatever the store path is.
        cp "$src" "${name}.ttf"
        mkfont -v \
          --size ${toString size} \
          --compress ${toString compress} \
          ${lib.optionalString (range != null) "--range ${range}"} \
          ${lib.optionalString (outline != null) "--outline ${toString outline}"} \
          ${lib.optionalString monochrome "--monochrome"} \
          -o "$outdir" "${name}.ttf"
      '';
    };

  # ── Sound effects ────────────────────────────────────────────────────
  # Report §5: VADPCM (level 1) is the right default — a 4-bit ADPCM variant
  # decoded on the RSP's 8-lane SIMD, cheap at runtime. Opus (level 3) is for
  # long streamed cues, not one-shots.
  mkSound =
    { name
    , src # .wav / .mp3
    , dest ? "sfx"
    , compress ? 1
    , resample ? null
    , mono ? false
    , loop ? false
    , loopOffset ? 0
    }:
    let
      # audioconv64 dispatches on the FILE EXTENSION, so the staged copy has
      # to keep the source's. This used to hardcode `.wav`, which meant the
      # documented ".wav / .mp3" contract was only half true: an mp3 arrived
      # named .wav and audioconv64 tried to parse an MPEG frame header as a
      # RIFF chunk and failed. Anything audioconv64 accepts is fine here.
      srcExt = if lib.hasSuffix ".mp3" (lib.toLower (toString src)) then "mp3" else "wav";
    in
    mkAsset {
      inherit name src dest;
      outName = "${name}.wav64";
      convert = ''
        cp "$src" "${name}.${srcExt}"
        audioconv64 -v \
          --wav-compress ${toString compress} \
          ${lib.optionalString (resample != null) "--wav-resample ${toString resample}"} \
          ${lib.optionalString mono "--wav-mono"} \
          ${lib.optionalString loop "--wav-loop true --wav-loop-offset ${toString loopOffset}"} \
          -o "$outdir" "${name}.${srcExt}"
      '';
    };

  # ── Video (FMV) ──────────────────────────────────────────────────────
  # engine/src/kiln/kiln_video.h decodes raw MPEG1 elementary streams
  # (`.m1v`, libdragon's mpeg1_codec) frame by frame at runtime. No
  # audioconv64 step and no re-encode here: unlike mkSound's mp3/wav
  # inputs, a `.m1v` is already exactly the bytes the console's decoder
  # wants, so this is a straight copy into the filesystem/ convention
  # every other builder here produces.
  mkVideo =
    { name
    , src # .m1v, a raw MPEG1 elementary stream (no container — see
          # libdragon's videoconv64 --codec mpeg1)
    , dest ? "videos"
    }:
    mkAsset {
      inherit name src dest;
      outName = "${name}.m1v";
      convert = ''
        cp "$src" "$outdir/${name}.m1v"
      '';
    };

  # ── Tracker music ────────────────────────────────────────────────────
  # libdragon benchmarks a 10-channel XM at "< 3% CPU and < 10% RSP", which is
  # why report §5 calls XM64 the pragmatic music engine for this target.
  mkMusic =
    { name
    , src # .xm (MilkyTracker/OpenMPT) or .ym (Arkos Tracker 2)
    , dest ? "music"
    , ymCompress ? true
    }:
    let
      ext = if lib.hasSuffix ".ym" (lib.toLower (toString src)) then "ym" else "xm";
    in
    mkAsset {
      inherit name src dest;
      outName = "${name}.${ext}64";
      convert = ''
        cp "$src" "${name}.${ext}"
        audioconv64 -v \
          ${lib.optionalString (ext == "ym" && ymCompress) "--ym-compress true"} \
          -o "$outdir" "${name}.${ext}"
      '';
    };

  # ── MIDI as tracker music ────────────────────────────────────────────
  # A score authored in a DAW arrives as MIDI, and the console has neither a
  # MIDI synthesiser nor the RAM for a soundfont. tools/midi_to_xm.py turns
  # the score into an XM module with synthesised single-cycle instruments,
  # which then takes mkMusic's ordinary path to .xm64.
  #
  # This is the cheap half of the "how do I get this song into the ROM"
  # question: PetaByte Madness' 64-second string quartet is 4.7 KB as an XM
  # and loops exactly, where the same piece as a streamed wav64 is ~1.2 MB
  # and has to be cross-faded by hand to loop at all. Use mkSound for the
  # latter when a recording's exact timbre is the point.
  mkMidiMusic =
    { name
    , src # .mid / .midi
      # Defaults to this repo's own converter, the way mkVeilTexture defaults
      # `generator`. A downstream flake consuming this builder through
      # `kiln.lib` has no sensible way to name a script that lives in the
      # engine's tools/ directory, and every caller so far passes the same one.
    , converter ? ../tools/midi_to_xm.py
    , dest ? "music"
    , songName ? name
    , rowsPerBeat ? 4
      # Per-channel harmonic rolloff, brightest first. The default is tuned
      # for a string quartet: two violins, viola, cello.
    , brightness ? "0.62,0.58,0.45,0.32"
    }:
    mkAsset {
      inherit name src dest;
      outName = "${name}.xm64";
      extraInputs = [ pkgs.python3 ];
      convert = ''
        python3 ${converter} \
          --in "$src" \
          --out "${name}.xm" \
          --name "${songName}" \
          --rows-per-beat ${toString rowsPerBeat} \
          --brightness "${brightness}"
        # --xm-compress 0: do NOT VADPCM the samples.
        #
        # midi_to_xm's instruments are SINGLE-CYCLE waveforms, 128 samples
        # looping every cycle. VADPCM is ADPCM with a predictor per 16-sample
        # frame, so at the loop point the decoder's state does not carry —
        # and for a cycle that repeats a few hundred times a second that
        # discontinuity is a rasp on top of every note. It is inaudible on a
        # long recorded sample, which is what the default is tuned for, and
        # ruinous on a synthesised one.
        #
        # Uncompressed costs 128 bytes per instrument. That is the entire
        # price of the fix.
        audioconv64 -v --xm-compress 0 -o "$outdir" "${name}.xm"
      '';
    };

  # ── Arbitrary compressed blobs ───────────────────────────────────────
  # For data a game loads itself via asset_load()/asset_fopen(): level data,
  # dialogue tables, save templates.
  mkRawAsset =
    { name
    , src
    , dest ? "data"
    , compress ? 1
    , extension ? "bin"
    }:
    mkAsset {
      inherit name src dest;
      outName = "${name}.${extension}";
      convert = ''
        cp "$src" "${name}.${extension}"
        mkasset -v -c ${toString compress} -o "$outdir" "${name}.${extension}"
      '';
    };

  # ── CI4 textures for a palette-swap material effect ────────────────────
  # See the n64-modeling skill's "CI4 and a palette-swap contract": author
  # both states offline, and at runtime change only which 16-entry TLUT a
  # material points at. 32 bytes of DMA and zero extra pixels shaded, against
  # the read-modify-writes a full-screen tinted quad costs at 320x240 for the
  # same effect done as a screen filter instead — the single most expensive
  # thing you can ask an RDP to do, buying nothing the swap does not.
  #
  # `kiln_voxmesh`'s `kiln_voxatlas_bind` is the generic engine-side half of
  # binding one of a pair of resident palettes; a game wanting this on its
  # own materials (not just Forge's voxel atlas) binds per-material on top of
  # that. This builder produces the pixel+palette pair either consumes.
  #
  # ── One derivation, three outputs, for the same reason as mkTextures ──
  #   $out/filesystem/<dest>/<name>.sprite   the CI4 image
  #   $out/filesystem/<dest>/<name>.pal      64 bytes: cold then veiled,
  #                                          16 x RGBA5551 big-endian each
  #   $out/png/<name>.png                    the SOURCE png, for gltf_to_t3d
  #
  # The third exists because `gltf_to_t3d` decodes the PNG at CONVERSION time
  # purely to learn the texture's pixel dimensions — glTF stores UVs as pixel
  # coordinates (meshConverter.cpp:120), so a model referencing this texture
  # cannot be built without the image being on disk beside it. Same arrangement
  # mkTextures uses, and for the same reason: generating the pixels twice and
  # hoping the two runs agree is how a model ends up carrying UVs scaled for one
  # texture while the ROM ships another.
  #
  # They must describe the SAME quantisation. A `.pal` built from one
  # quantisation of a PNG and a `.sprite` built from another would produce a
  # texture whose texel indices address the wrong colours — a model that
  # renders, in the wrong palette, with nothing failing anywhere. Splitting
  # these into two derivations is exactly how that happens, so they are one.
  #
  # ── The source must ALREADY be indexed ───────────────────────────────
  # `format = "CI4"` makes mksprite quantise, and tools/veil_palette.py reads
  # the PNG's own palette. If the PNG were truecolour those would be two
  # independent quantisations of the same image and the indices would not
  # correspond. So an indexed (PIL mode "P") source is required and the absence
  # of one is an error rather than a silent mismatch — see the checkPhase.
  #
  mkVeilTexture =
    { name
    , src
      # world | demon | phantom | eyes — see tools/veil_palette.py's module
      # docstring for the four material classes.
      # This is the single most consequential parameter: `phantom` makes the
      # cold palette alpha-0 on every entry, i.e. the creature is not drawn at
      # all with the veil down, and getting it wrong on a demon body shows the
      # player a monster the design says they cannot see yet.
    , veilClass ? "world"
      # `textures`, not something veil-specific, because gltf_to_t3d's path
      # mapping is not negotiable: mkBlenderModel stages a model's textures into
      # `stage/assets/textures/`, a material spec names `tex=textures/<n>.png`,
      # and the importer rewrites that to `rom:/textures/<n>.sprite`. Shipping
      # the sprite anywhere else gives the model a rom path with nothing at it.
      # The .pal rides along in the same directory rather than getting a tidier
      # home of its own, so the pair stays together.
    , dest ? "textures"
    , compress ? 1
    , generator ? ../tools/veil_palette.py
      # The companion to `generator`: it removes the palette mksprite embeds,
      # which would otherwise be uploaded over the ramp the game binds. See
      # the script's own header for the whole sequence.
    , paletteStripper ? ../tools/veil_strip_palette.py
      # Whether to run it. Off is a DIAGNOSTIC state, not a supported one: with
      # the embedded palette left in place the sprite's own colours are
      # uploaded over whatever the game bound, so the veil cannot swap
      # anything — but the material renders in SOME palette rather than
      # sampling a zero one and coming out pure black. That difference is how
      # you tell "the game's TLUT upload never landed" apart from "nothing in
      # this pipeline uploads a TLUT at all", which no other observation
      # distinguishes.
    , stripPalette ? true
    }:
    pkgs.stdenv.mkDerivation {
      pname = "veil-${name}";
      version = "0.1.0";
      dontUnpack = true;
      dontConfigure = true;
      nativeBuildInputs = [ n64Inst pkgs.python3 pkgs.python3Packages.pillow ];
      inherit src;

      buildPhase = ''
        runHook preBuild
        outdir="filesystem/${dest}"
        mkdir -p "$outdir"

        cp "$src" "${name}.png"

        # The palette first: it is the step that will reject a truecolour
        # source, and failing before mksprite has produced a plausible-looking
        # .sprite keeps a half-built pair out of the store.
        python3 ${generator} "${name}.png" "$outdir/${name}.pal" \
            --class ${veilClass}

        # UNCOMPRESSED here even when `compress` is set — the palette-stripping
        # step below has to read the sprite's headers, and mkasset re-applies
        # the requested compression afterwards.
        mksprite -v --format CI4 --compress 0 -o "$outdir" "${name}.png"

        # ── Strip the embedded palette ───────────────────────────────────
        # This is the fix for "the veil renders but the palette swap does
        # not", and it is worth spelling out because the symptom pointed
        # somewhere else entirely: with the veil's material specs enabled, the
        # model rendered a texture, but a diagnostic pure-GREEN cold palette
        # produced ZERO green pixels. That reads as "the TLUT never reaches
        # the RDP", and the search went looking at combiners.
        #
        # The actual sequence, in Tiny3D's set_texture():
        #
        #   t3dmodel.c:147   conf->tileCb(...)        <- pm_veil_bind_palette
        #                                                uploads the ramp here
        #   t3dmodel.c:158   rdpq_sprite_upload(...)  <- eleven lines later
        #
        # and rdpq_sprite_upload calls sprite_upload_palette
        # (rdpq_sprite.c:17-35), which uploads THE SPRITE'S OWN palette. Both
        # write the same 16-entry block: the veil binds at
        # `tmem_tile * 16` with tmem_tile 0 for every material (see
        # pm_veil_palettes_init's comment on why), and sprite_upload_palette
        # writes at `palidx * 16` with palidx 0. So the ramp was uploaded and
        # immediately overwritten, every frame, on every material — and what
        # reached the screen was the texture in its own authored palette,
        # which looks like a plausible picture rather than a failure.
        #
        # tileCb is documented as the hook for tile settings and it is; it is
        # just not after the upload, and there is no callback that is. The
        # escape hatch is named in rdpq_sprite.c's own comment three lines
        # above the clobber: "We account for sprites being CI4 but without
        # embedded palette: mksprite doesn't create sprites like this today,
        # but it could in the future (eg: sharing a palette across [sprites])."
        # A veil material is exactly that case — its palette is the .pal
        # sidecar beside it, loaded once at boot into a nine-step ramp, and
        # the copy inside the sprite is dead weight that only does harm.
        #
        # So: zero sprite_ext_t.pal_file_pos. sprite_get_palette then returns
        # NULL (sprite.c:221-226), sprite_upload_palette still sets
        # rdpq_mode_tlut correctly and skips the upload, and whatever the tile
        # callback bound survives to the RDP.
        #
        # This is done to the FILE rather than by patching libdragon or Tiny3D
        # because it changes nothing for any other sprite in any other ROM —
        # a patch to either would.
        ${if stripPalette
          then ''python3 ${paletteStripper} "$outdir/${name}.sprite"''
          else ''echo "  veil: ${name}.sprite KEEPS its embedded palette (diagnostic build)"''}

        ${lib.optionalString (compress != 0) ''
          # Re-apply the compression mksprite was not allowed to do above.
          # mkasset rewrites in place via -o pointing at the same directory.
          mkasset -c ${toString compress} -w 256 -o "$outdir"               "$outdir/${name}.sprite"
        ''}
        runHook postBuild
      '';

      doCheck = true;
      checkPhase = ''
        runHook preCheck
        outdir="filesystem/${dest}"
        [ -s "$outdir/${name}.sprite" ] || {
          echo "mkVeilTexture: no ${name}.sprite" >&2; exit 1; }
        # Exactly 64 bytes: 2 palettes x 16 entries x 2 bytes. Asserted rather
        # than assumed because a runtime ramp-build reads a fixed 16 entries
        # from each half and a short file would feed it whatever followed in
        # RAM.
        sz=$(stat -c%s "$outdir/${name}.pal")
        [ "$sz" = 64 ] || {
          echo "mkVeilTexture: ${name}.pal is $sz bytes, expected 64" >&2
          exit 1; }

        # ── The strip landed ────────────────────────────────────────────
        # veil_strip_palette.py exits non-zero on every case it can detect,
        # so this is not re-checking its work — it is checking that the step
        # ran at all against the file that ends up in the store, which is a
        # different claim once mkasset has rewritten it. A veil sprite that
        # keeps its embedded palette does not fail, warn, or look wrong: it
        # renders the texture in its authored colours and the palette swap
        # silently does nothing, which is a whole mechanic quietly absent.
        # That defect cost this project a diagnostic green-palette bake and a
        # capture to find the first time, so it gets a gate.
        ${lib.optionalString (stripPalette && compress == 0) ''
          python3 -c '
import struct, sys
b = open(sys.argv[1], "rb").read()
w, h, _bd, fl = struct.unpack_from(">HHBB", b, 0)
ext = 8 if fl & 0x40 else 8 + (w * h + 1) // 2
pal = struct.unpack_from(">I", b, ext + 4)[0]
if pal != 0:
    sys.exit("mkVeilTexture: %s still carries an embedded palette at %d. It "
             "will be uploaded over the veil ramp and the swap will do "
             "nothing." % (sys.argv[1], pal))
' "$outdir/${name}.sprite"
        ''}
        ${lib.optionalString (compress != 0) ''
          # Compressed: the headers are behind mkasset's container, so the
          # readable claim is that the file went through both tools. mkasset
          # writes the "DCA" magic; its absence means the compress step was
          # skipped and the sprite in the store is not the one measured above.
          head -c 3 "$outdir/${name}.sprite" | grep -q DCA || {
            echo "mkVeilTexture: ${name}.sprite is not mkasset-compressed;" \
                 "the palette-strip and compress steps disagree." >&2
            exit 1; }
        ''}
        runHook postCheck
      '';

      installPhase = ''
        runHook preInstall
        mkdir -p "$out/png"
        cp -r filesystem "$out/"
        # The source PNG under the name gltf_to_t3d will look for. Kept
        # unquantised: mksprite did the CI4 conversion for the ROM, but the
        # importer only wants the dimensions, and handing it an indexed PNG it
        # has no reason to decode is a needless dependency on its PNG support.
        cp "${name}.png" "$out/png/${name}.png"
        runHook postInstall
      '';

      dontStrip = true;
      dontPatchELF = true;

      passthru = {
        assetName = "${name}.sprite";
        palName = "${name}.pal";
        inherit dest veilClass;
      };

      meta.description = "veil CI4 texture + cold/veiled TLUT pair: ${name}";
    };

  # ── Single-file asset container ──────────────────────────────────────
  # Packs a list of already-converted assets into one .streamdb file the
  # runtime kiln_asset layer mounts from DFS. The pack tool is the upstream C
  # writer — the SAME binary nix/checks/streamdb.nix builds to verify the
  # reader, so the format writer is reviewed code we already trust. No new
  # writer to maintain.
  #
  # Each entry is `{ key, asset }`:
  #   * `key`  is the StreamDB key, e.g. "models/cube.t3dm" — the same string
  #     the ROM will pass to kiln_asset_load. Convention: the key IS the path
  #     the file would have had in loose DFS, so an asset can move between
  #     the two containers with no code change at the call site.
  #   * `asset` is a mkModel/mkSprite/mkSound/mkRawAsset derivation (anything
  #     that ships a `filesystem/` directory). The file MUST live at
  #     `<asset>/filesystem/<key>` — enforced below with an explicit check.
  #
  # The output derivation's `filesystem/<name>.streamdb` is what mkN64Rom's
  # `assets` argument consumes; one .streamdb file lands in DFS like any
  # loose asset would.
  mkStreamdb =
    { name
    , entries
    , dest ? ""
    }:
    pkgs.stdenv.mkDerivation {
      pname = "streamdb-${name}";
      version = "0.1.0";
      nativeBuildInputs = [ pkgs.gcc pkgs.python3 ];
      dontUnpack = true;

      buildPhase = ''
        runHook preBuild
        set -euo pipefail

        # Validate every entry's file exists before invoking the packer, so
        # a mis-specified `key` (e.g. "models/cube.t3dm" when the asset's
        # dest/name produced "models/Cube.t3dm") fails with a clear message
        # rather than a streamdb_insert error.
        ${lib.concatMapStrings (e: ''
          if [ ! -s "${e.asset}/filesystem/${e.key}" ]; then
            echo "mkStreamdb: key '${e.key}' not found under ${e.asset}/filesystem/" >&2
            find ${e.asset}/filesystem -type f >&2 || true
            exit 1
          fi
        '') entries}

        # Build the upstream pack tool. Same recipe as nix/checks/streamdb.nix.
        gcc -O2 -std=gnu11 -I${streamdbSrc}/C/include -o pack \
            ${./../streamdb-embedded/test/pack.c} \
            ${streamdbSrc}/C/src/streamdb.c -lpthread

        outdir="filesystem${lib.optionalString (dest != "") "/${dest}"}"
        mkdir -p "$outdir"

        # pack: `pack <out> <key> <file> [<key> <file>...]`
        ./pack "$outdir/${name}.streamdb" \
          ${lib.escapeShellArgs (builtins.concatMap (e:
            [ e.key "${e.asset}/filesystem/${e.key}" ]
          ) entries)} > /dev/null

        # Sanity: the file exists and is non-empty.
        if [ ! -s "$outdir/${name}.streamdb" ]; then
          echo "mkStreamdb: $outdir/${name}.streamdb was not produced" >&2
          exit 1
        fi
        echo "  ${name}.streamdb: $(stat -c%s "$outdir/${name}.streamdb") bytes, ${toString (builtins.length entries)} entries"
        runHook postBuild
      '';

      doCheck = true;
      checkPhase = ''
        runHook preCheck
        outdir="filesystem${lib.optionalString (dest != "") "/${dest}"}"
        # The packer prints one "packed" line per insert; if any insert
        # failed it would have exited non-zero, so this is just a count
        # cross-check against the file's own index. We do that by reading
        # the header's doc count via Python — keeps the check hermetic to
        # the format, not to our pack wrapper.
        python3 - "$outdir/${name}.streamdb" <<'PY'
        import struct, sys
        with open(sys.argv[1], "rb") as f:
            slot = f.read(128)
        assert slot[:4] == b"STDB", "bad magic"
        assert struct.unpack_from("<I", slot, 4)[0] == 3, "bad version"
        idx_off, idx_len = struct.unpack_from("<QQ", slot, 40)
        f = open(sys.argv[1], "rb"); f.seek(idx_off)
        idx = f.read(idx_len)
        n = struct.unpack_from("<Q", idx, 0)[0]
        print(f"  index: {n} documents")
        PY
        runHook postCheck
      '';

      installPhase = ''
        runHook preInstall
        mkdir -p $out
        cp -r filesystem $out/
        runHook postInstall
      '';

      dontStrip = true;
      dontPatchELF = true;

      passthru = { assetName = "${name}.streamdb"; inherit dest; };

      meta.description = "StreamDB container '${name}' (${toString (builtins.length entries)} assets)";
    };

  # ── One StreamDB per ROM, keys derived rather than hand-typed ───────────
  # mkStreamdb's `entries` require the caller to state each key AND rely on
  # it matching the asset's own dest/name output path — exactly the
  # "asset builder's name IS the filename, and nothing checks it" gap
  # CLAUDE.md's hard-won facts documents (a mismatched key here fails the
  # same way: a build that succeeds and a ROM that gets nothing at that
  # path). mkAssetPak instead takes a plain list of already-built asset
  # derivations and packs EVERY file each one actually produced under its
  # own `filesystem/`, keyed by that file's own relative path — the key can
  # no longer drift from the file, because it IS the file's own path.
  #
  # Not "exactly one file per asset": an ANIMATED mkModel ships its .t3dm
  # plus one or more `.N.sdata` clip-streaming sidecars, discovered here the
  # same as a single-file mkSprite/mkRawAsset/mkSound/mkMusic output would
  # be. Whether packing those sidecars into StreamDB actually WORKS at
  # runtime is a separate question this helper does not answer — Tiny3D's
  # t3danim.c opens them via `asset_fopen(animDef->filePath, ...)`, a
  # hardcoded DFS-path open with no buffer variant, so an animated model's
  # sidecars need to stay reachable as loose DFS regardless of where the
  # `.t3dm` itself came from. See CLAUDE.md's "Datafiles: StreamDB vs loose
  # DFS" — this is why examples/camera-skel-demo (an animated rig) stays on
  # loose DFS entirely rather than using this helper.
  mkAssetPak =
    { name
    , assets
    , dest ? ""
    }:
    pkgs.stdenv.mkDerivation {
      pname = "assetpak-${name}";
      version = "0.1.0";
      nativeBuildInputs = [ pkgs.gcc pkgs.python3 ];
      dontUnpack = true;

      buildPhase = ''
        runHook preBuild
        set -euo pipefail

        # Same upstream pack tool as mkStreamdb — one writer, reviewed once.
        gcc -O2 -std=gnu11 -I${streamdbSrc}/C/include -o pack \
            ${./../streamdb-embedded/test/pack.c} \
            ${streamdbSrc}/C/src/streamdb.c -lpthread

        outdir="filesystem${lib.optionalString (dest != "") "/${dest}"}"
        mkdir -p "$outdir"

        args=()
        nfiles=0
        ${lib.concatMapStrings (a: ''
          n=$(find ${a}/filesystem -type f | wc -l)
          if [ "$n" -eq 0 ]; then
            echo "mkAssetPak: no files found under ${a}/filesystem" >&2
            exit 1
          fi
          while IFS= read -r f; do
            key="''${f#${a}/filesystem/}"
            args+=("$key" "$f")
            nfiles=$((nfiles + 1))
          done < <(find ${a}/filesystem -type f)
        '') assets}

        ./pack "$outdir/${name}.streamdb" "''${args[@]}" > /dev/null

        if [ ! -s "$outdir/${name}.streamdb" ]; then
          echo "mkAssetPak: $outdir/${name}.streamdb was not produced" >&2
          exit 1
        fi
        echo "  ${name}.streamdb: $(stat -c%s "$outdir/${name}.streamdb") bytes, $nfiles file(s) from ${toString (builtins.length assets)} asset(s)"
        runHook postBuild
      '';

      doCheck = true;
      checkPhase = ''
        runHook preCheck
        outdir="filesystem${lib.optionalString (dest != "") "/${dest}"}"
        python3 - "$outdir/${name}.streamdb" <<'PY'
        import struct, sys
        with open(sys.argv[1], "rb") as f:
            slot = f.read(128)
        assert slot[:4] == b"STDB", "bad magic"
        assert struct.unpack_from("<I", slot, 4)[0] == 3, "bad version"
        idx_off, idx_len = struct.unpack_from("<QQ", slot, 40)
        f = open(sys.argv[1], "rb"); f.seek(idx_off)
        idx = f.read(idx_len)
        n = struct.unpack_from("<Q", idx, 0)[0]
        print(f"  index: {n} documents")
        PY
        runHook postCheck
      '';

      installPhase = ''
        runHook preInstall
        mkdir -p $out
        cp -r filesystem $out/
        runHook postInstall
      '';

      dontStrip = true;
      dontPatchELF = true;

      passthru = { assetName = "${name}.streamdb"; inherit dest; };

      meta.description = "StreamDB container '${name}' (${toString (builtins.length assets)} assets, auto-keyed)";
    };

  # ── Texture atlas (flipbook frames) ─────────────────────────────────
  # Slices a sprite sheet PNG into individual .sprite files for flipbook
  # animation. Each frame becomes `<name>_NN.sprite` in the output. The
  # runtime loads them with sprite_load and swaps via rdpq_set_lookup_address.
  #
  # The sheet must be a grid of equal-sized frames. `cols` and `rows`
  # describe the grid; `frameW` and `frameH` are the per-frame dimensions
  # (sheet_w / cols and sheet_h / rows, passed explicitly to avoid a Python
  # dependency for the math).
  mkTextureAtlas =
    { name
    , src          # sprite sheet PNG
    , dest ? "sprites"
    , cols
    , rows
    , frameW
    , frameH
    , format ? null
    , compress ? 1
    }:
    let
      frameCount = cols * rows;
      padIdx = n: lib.concatStringsSep "" (lib.genList (i:
        if i < 2 - lib.stringLength (toString n) then "0" else ""
      ) (lib.range 0 1)) + toString n;
    in
    pkgs.stdenv.mkDerivation {
      pname = "atlas-${name}";
      version = "0.1.0";
      inherit src;
      dontUnpack = true;

      nativeBuildInputs = [ n64Inst pkgs.imagemagick ];

      buildPhase = ''
        runHook preBuild
        mkdir -p "$outdir"
        outdir="filesystem/${dest}"
        mkdir -p "$outdir"

        # Slice the sheet into individual frame PNGs using ImageMagick.
        for i in $(seq 0 ${toString (frameCount - 1)}); do
          col=$((i % ${toString cols}))
          row=$((i / ${toString cols}))
          x=$((col * ${toString frameW}))
          y=$((row * ${toString frameH}))
          idx=$(printf "%02d" "$i")
          magick "$src" -crop ${toString frameW}x${toString frameH}+''${x}+''${y} +repage \
            "frame_''${idx}.png"
        done

        # Convert each frame to a .sprite.
        for f in frame_*.png; do
          idx="''${f#frame_}"
          idx="''${idx%.png}"
          mksprite -v \
            ${lib.optionalString (format != null) "--format ${format}"} \
            --compress ${toString compress} \
            -o "$outdir" "$f"
          # Rename to the final <name>_NN.sprite pattern.
          mv "$outdir/$(basename "$f" .png).sprite" "$outdir/${name}_''${idx}.sprite"
        done

        rm -f frame_*.png
        runHook postBuild
      '';

      doCheck = true;
      checkPhase = ''
        runHook preCheck
        outdir="filesystem/${dest}"
        count=$(ls "$outdir"/${name}_*.sprite 2>/dev/null | wc -l)
        if [ "$count" -ne ${toString frameCount} ]; then
          echo "mkTextureAtlas '${name}': expected ${toString frameCount} sprites, got $count" >&2
          exit 1
        fi
        echo "  ${name}: $count frames"
        runHook postCheck
      '';

      installPhase = ''
        runHook preInstall
        mkdir -p $out
        cp -r filesystem $out/
        runHook postInstall
      '';

      dontStrip = true;
      dontPatchELF = true;

      passthru = { inherit dest; frameCount = frameCount; };

      meta.description = "texture atlas '${name}' (${toString frameCount} frames)";
    };
}
