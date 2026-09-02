# SPDX-License-Identifier: MIT
{
  description = "Kiln — a Nix build system for Nintendo 64 / ModRetro M64 software";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";

    # libdragon is the SDK (not libultra): modern GCC, the RSP-accelerated
    # mixer, wav64/VADPCM/Opus, XM64, rspq, and the host tools including
    # audioconv64. Pinned as a plain source tree so the rev is explicit.
    #
    # The `preview` branch, not the default `trunk`. This is not optional:
    # Tiny3D's README states it "requires libdragon, specifically the preview
    # branch", and it typedefs its core math types straight off libdragon's
    # fast-math vectors (`typedef fm_vec3_t T3DVec3;`). trunk's fmath.h has only
    # the scalar functions, so on trunk those typedefs silently resolve to
    # nothing and every downstream use fails with errors that look unrelated
    # ("control reaches end of non-void function", "request for member 'm' in
    # something not a structure"). preview is also where third-party N64
    # libraries generally target.
    libdragon = {
      url = "github:DragonMinded/libdragon/preview";
      flake = false;
    };

    # SummerCart64 — provides sc64deployer (ROM upload, debugf stdio,
    # IS-Viewer64, and the AUX PC<->N64 message channel the report's §6
    # networking design uses as its control tunnel).
    summercart64 = {
      url = "github:Polprzewodnikowy/SummerCart64";
      flake = false;
    };

    # StreamDB — the data-management layer. Upstream's C edition cannot run on
    # bare metal (pthreads, flock, fsync, and a 1 KB-per-node trie), so
    # streamdb-embedded/ is a read-only implementation of the same v3 format
    # with a memory strategy suited to 4 MB of RAM.
    streamdb = {
      url = "github:ALH477/DeMoD-StreamDB";
      flake = false;
    };

    # Tiny3D — the 3D half of the engine (RSP-accelerated matrices, models,
    # skinning, particles). libdragon's rdpq provides the 2D half.
    tiny3d = {
      url = "github:HailToDodongo/tiny3d";
      flake = false;
    };

    # N64-UNFLoader — cross-flashcart loader + debugf stdio bridge. ED64 fallback
    # when the connected cart is an FT245R (VID 0403, PID 6001) rather than SC64.
    unfloader-src = {
      url = "github:buu342/N64-UNFLoader";
      flake = false;
    };

    # Claude Code CLI, packaged for Nix. Used only by packages.dev-image (see
    # nix/dev-image.nix) — not part of the N64 build at all.
    claude-code-nix.url = "github:sadjow/claude-code-nix";

    # Builds a NixOS system config into a docker-loadable image. Also only
    # used by packages.dev-image.
    nixos-generators = {
      url = "github:nix-community/nixos-generators";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, flake-utils, libdragon, summercart64, tiny3d, streamdb, unfloader-src, claude-code-nix, nixos-generators }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        # The cross toolchain. Owns the whole toolchain strategy; see the file.
        toolchain = import ./nix/toolchain.nix { inherit nixpkgs pkgs system; };

        # libdragon, installed into a store path used as $N64_INST.
        # libdragon's own fast-math library, built for the host. The first
        # brick of the PC target: the engine's fm_vec3_t and 17 fm_* calls are
        # now the REAL ones natively, not a hand-copy. See nix/host-math.nix.
        hostMath = import ./nix/host-math.nix {
          inherit pkgs;
          src = libdragon;
        };

        # The host tier, once, for any toolchain. `hostTargets.targets` names
        # the compilers: native, wasm32, and (Linux only, not gated) aarch64
        # and riscv64 under qemu-user. Each target carries its own hostMath,
        # its own libkilnhost.a and libkiln.a, and the mkCheck/mkProgram that
        # build against them. See nix/host.nix for why the recipe moved out of
        # the seven checks that used to each carry a copy.
        hostTargets = import ./nix/host.nix {
          inherit pkgs;
          libdragonSrc = libdragon;
          engineSrc = ./engine;
          platHost = ./plat/host;
          platShell = ./plat/shell;
          streamdbSrc = ./streamdb-embedded;
          webShellHtml = ./plat/shell/kiln_web_shell.html;
        };
        hostNative = hostTargets.targets.native;
        hostWasm   = hostTargets.targets.wasm32;

        # aarch64 and riscv64 are real targets and are deliberately NOT in the
        # gate set: a cross toolchain plus qemu-user is a ~170 MB fetch, and a
        # pre-push gate has no business pulling that. They are one command —
        # `nix build .#host-arch-aarch64` — and they run the SAME check bodies
        # against the SAME reference files as x86_64 and wasm32 do, under
        # qemu-user. Three renders is enough to catch an architecture
        # disagreeing: the 2D pass, the whole frame bracket, and a real .t3dm.
        archProof = t: pkgs.linkFarm "host-arch-${t.name}" [
          { name = "gui";
            path = import ./nix/checks/kiln-gui.nix { inherit pkgs; target = t; }; }
          { name = "scene";
            path = import ./nix/checks/kiln-scene.nix { inherit pkgs; target = t; }; }
          { name = "model";
            path = import ./nix/checks/kiln-model.nix {
              inherit pkgs n64Inst; target = t; cubeGltf = ./assets/cube.gltf; }; }
        ];

        libdragon-sdk = import ./nix/libdragon.nix {
          inherit pkgs toolchain;
          src = libdragon;
        };

        # Tiny3D, installed with libdragon's layout so the two can be merged.
        tiny3d-sdk = import ./nix/tiny3d.nix {
          inherit pkgs toolchain;
          src = tiny3d;
          libdragon = libdragon-sdk;
        };

        # Data management: StreamDB v3 reader, on-console. Built against
        # libdragon alone (it needs only n64.mk and the DFS backend); keeping
        # it off tiny3d lets it land in n64InstBase, which the engine needs
        # because kiln_asset.c includes <streamdb_embedded.h>.
        streamdb-emb = import ./nix/streamdb.nix {
          inherit pkgs toolchain;
          libdragon = libdragon-sdk;
          src = ./streamdb-embedded;
        };

        # Prefix stage 1: what the engine itself compiles against. Now
        # includes streamdb-emb so kiln_asset.c can find streamdb_embedded.h.
        n64InstBase = import ./nix/n64-inst.nix {
          inherit pkgs;
          libdragon = libdragon-sdk;
          extraLibs = [ tiny3d-sdk streamdb-emb ];
        };

        # The Kiln engine — 3D on Tiny3D, 2D GUI on rdpq, asset layer on
        # streamdb-embedded.
        kiln-engine = import ./nix/engine.nix {
          inherit pkgs toolchain n64InstBase;
          src = ./engine;
        };

        # The asset pipeline: wraps the host tools the merged prefix already
        # ships (gltf_to_t3d, mksprite, mkfont, audioconv64, mkasset) so
        # projects get built assets instead of hand-built geometry, plus
        # mkStreamdb to pack any of them into a single .streamdb container.
        assetLib = import ./nix/assets.nix {
          inherit pkgs;
          n64Inst = n64InstBase;
          streamdbSrc = streamdb;
        };

        # Demo assets, converted from the hand-authored sources in assets/.
        # Exercised by assets-demo below and by the determinism check.
        demoModel = assetLib.mkModel {
          name = "cube";
          src = ./assets/cube.gltf;
          # Hand-authored glTF, not exported by Blender's fast64 add-on, so it
          # carries none of the custom material properties gltf_to_t3d expects
          # (it aborts with "Material has no fast64 data!" otherwise). Real
          # models come out of the fast64 pipeline and won't need this.
          ignoreMaterials = true;
          # The source cube is a 1x1x1 Blender unit cube; the default
          # baseScale of 64 turns that into a 64-unit cube that overfills the
          # frustum at the demo's camera distance. 28 matches examples/engine's
          # hand-built cube (half-extent 14) for a comparable picture.
          baseScale = 28;
        };
        demoSprite = assetLib.mkSprite {
          name = "logo";
          src = ./assets/logo.png;
          format = "RGBA32";
        };
        demoSound = assetLib.mkSound {
          name = "blip";
          src = ./assets/blip.wav;
        };

        # Footstep sample for the Phase 3 surface/shader demo. Same mkSound
        # path as blip; ships at rom:/sfx/step.wav64 so clip-demo can load it
        # alongside blip and pick one per surface.
        stepSound = assetLib.mkSound {
          name = "step";
          src = ./assets/step.wav;
        };

        # A 2-bone rigged/skinned + animated test model (tools/gen_skel_gltf.py)
        # for camera-skel-demo. Same ignoreMaterials reasoning as demoModel —
        # hand-authored, not a fast64 export. baseScale 32 (half the default
        # 64) keeps the ~1-2 Blender-unit rig in the same size range as
        # examples/actors-demo's hand-built cubes (half-extent 8-14).
        skelModel = assetLib.mkModel {
          name = "rig";
          src = ./assets/skel_test.gltf;
          ignoreMaterials = true;
          baseScale = 32;
        };

        # ── StreamDB-destined demo assets ──────────────────────────────
        # Three assets packed into a single .streamdb, exercising every
        # kiln_asset accessor: kiln_asset_model (cube.t3dm), kiln_asset_sprite
        # (logo.sprite), and kiln_asset_load on a raw level-layout blob.
        #
        # IMPORTANT: compress = 0 across the board. kiln_asset_load returns
        # the StreamDB payload bytes verbatim — there is no asset_load in the
        # path to decompress them, so a mkasset-compressed .t3dm/.sprite
        # would arrive at t3d_model_load_buf / sprite_load_buf still
        # compressed and fail to parse. Loose-DFS assets-demo uses
        # compress = 2/1 because t3d_model_load / sprite_load call asset_load
        # internally; the StreamDB path bypasses asset_load, so the
        # compression step must be skipped at build time too. The .streamdb
        # container itself is the single compressed artefact if you want
        # compression — apply it there, not at the per-asset level.
        sdModel = assetLib.mkModel {
          name = "cube";
          src = ./assets/cube.gltf;
          ignoreMaterials = true;
          baseScale = 28;
          compress = 0;
        };
        sdSprite = assetLib.mkSprite {
          name = "logo";
          src = ./assets/logo.png;
          format = "RGBA32";
          compress = 0;
        };
        # A level-layout blob: 4-byte magic 'KLNL' + u32 spawn-count + N vec3
        # spawn points. Generated deterministically (no binary checked in).
        introLevel = pkgs.runCommandLocal "intro-level.bin" {
          nativeBuildInputs = [ pkgs.python3 ];
        } ''
          python3 -c '
          import struct, sys
          spawns = [(0,0,0),(10,0,10),(-10,0,10),(0,5,0)]
          with open(sys.argv[1], "wb") as f:
              f.write(b"KLNL")
              f.write(struct.pack("<I", len(spawns)))
              for x,y,z in spawns:
                  f.write(struct.pack("<fff", x, y, z))
          ' $out
        '';
        sdLevel = assetLib.mkRawAsset {
          name = "intro";
          src = introLevel;
          dest = "levels";
          compress = 0;
          extension = "bin";
        };
        # Loose Quake .map shipped to rom:/maps/ for kiln_map at runtime.
        quakeMap = assetLib.mkRawAsset {
          # The NAME IS THE FILENAME. It was "quake-test-map", shipping
          # maps/quake-test-map.map, while examples/map-demo/main.c:89 opens
          # rom:/maps/quake_test.map — so the map never loaded, exactly as
          # CLAUDE.md's "an asset builder's `name` is the FILENAME" entry
          # describes. It went unnoticed for as long as it did because the ROM
          # had no filesystem at all (see the Makefile), which failed first.
          name = "quake_test";
          src = ./assets/quake_test.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };
        # Two-room + enemies test map for examples/oot-demo. Same raw-asset
        # path as quakeMap so kiln_map reads it via rom:/maps/oot_test.map —
        # which this entry CLAIMED and did not do: the name was "oot-test-map",
        # so it shipped maps/oot-test-map.map while
        # examples/oot-demo/main.c:222 asked for oot_test.map. A comment
        # asserting the path is not the same as the name producing it.
        ootMap = assetLib.mkRawAsset {
          name = "oot_test";
          src = ./assets/oot_test.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };
        demoStreamdb = assetLib.mkStreamdb {
          name = "assets";
          entries = [
            { key = "models/cube.t3dm";    asset = sdModel; }
            { key = "sprites/logo.sprite"; asset = sdSprite; }
            { key = "levels/intro.bin";    asset = sdLevel; }
          ];
        };

        # assets-demo's StreamDB pak: reuses sdModel/sdSprite (the same
        # compress=0 twins demoStreamdb already packs) via mkAssetPak instead
        # of mkStreamdb's hand-typed `entries` — demonstrating the auto-keyed
        # helper on real, already-defined assets rather than new content.
        # demoSound stays loose DFS: see CLAUDE.md's "Datafiles: StreamDB vs
        # loose DFS" for why audio can't go through StreamDB at all today.
        assetsDemoPak = assetLib.mkAssetPak {
          name = "assets-demo";
          assets = [ sdModel sdSprite ];
        };

        # openworld-demo's tile mesh, StreamDB-packed so kiln_streamio can
        # load it through kiln_asset_model + kiln_cache instead of the
        # hand-built 2-vert stub the demo used before it had a real streaming
        # pacer to exercise. One shared model at one key — the point of the
        # demo is the pacer's priority/budget admission across many
        # simultaneous tile requests, not per-tile unique geometry.
        owTileModel = assetLib.mkModel {
          name = "tile";
          src = ./assets/cube.gltf;
          ignoreMaterials = true;
          baseScale = 24;
          compress = 0;
        };
        owStreamdb = assetLib.mkAssetPak {
          name = "openworld";
          assets = [ owTileModel ];
        };

        # Geometry authoring. Owns the whole Blender strategy; see the file for
        # why Fast64 is deliberately not vendored.
        blenderLib = import ./nix/blender.nix {
          inherit pkgs;
          n64Inst = n64InstBase;
        };

        # Procedural textures. One derivation feeds two consumers: the PNGs
        # gltf_to_t3d decodes at conversion time to learn UV pixel dimensions,
        # and the .sprites the ROM ships. See nix/assets.nix.
        textures = assetLib.mkTextures { name = "kiln"; };

        # ── Test geometry ─────────────────────────────────────────────
        # Each model isolates one class of renderer bug; tools/blender/models.py
        # documents which. Untextured models go through the `shade` preset,
        # which reproduces libdragon's RDPQ_COMBINER_SHADE exactly — the same
        # combiner kiln_scene_begin() already sets, so they compose with
        # hand-built geometry in the same pass.
        mkTestModel = model: extra: blenderLib.mkBlenderModel ({
          name = model;
          script = "models.py";
        } // extra);

        testModels =
          # Vertex-coloured. No texture means no TMEM and no tile setup, and
          # the `shade` preset leaves the scene's own RDP state alone.
          builtins.listToAttrs (map
            (n: { name = n; value = mkTestModel n { }; })
            [ "axes" "cube" "sphere" "cylinder" "cone" "torus" "stress" ])
          // {
            checker = mkTestModel "checker" {
              inherit textures;
              materials = [
                "CheckerWrapMat=tex0_shade,tex=textures/checker.i8.png,size=32"
                # Same texture, mirrored repeat — the difference between the two
                # quads is the whole point of the model.
                "CheckerMirrorMat=tex0_shade,tex=textures/checker.i8.png,size=32,mirror=true"
              ];
            };
            uvsphere = mkTestModel "uvsphere" {
              inherit textures;
              materials = [
                "GridMat=tex0_shade,tex=textures/grid.rgba16.png,size=32"
              ];
            };
          };

        # The rigged/animated reference — the only thing here exercising
        # Tiny3D's skinning + animation (t3dskeleton.h/t3danim.h) through the
        # Blender-authoring path (kiln_skel.h's runtime side is exercised
        # separately by examples/camera-skel-demo's hand-authored rig).
        # tools/blender/goblin.py documents why every part is rigidly bound
        # to exactly one bone.
        goblinModel = blenderLib.mkBlenderModel {
          name = "goblin";
          script = "goblin.py";
          animated = true;
        };

        # The hero prop: not a test shape, but a piece of content authored the
        # way a game's content is — one silhouette from six interpenetrating
        # parts, shaded entirely by COLOR_0 through the same `shade` combiner
        # kiln_scene_begin() already sets, so it composes with hand-built
        # geometry in one pass and needs no TMEM. tools/blender/interceptor.py
        # documents the orientation and the budget.
        interceptorModel = blenderLib.mkBlenderModel {
          name = "interceptor";
          script = "interceptor.py";
        };

        # Cinematic-demo extras: a service droid (rigged+animated, two bones
        # driving a wave animation), an approaching alien (taller, six-legged
        # silhouette, head-bob animation), and the hangar.map that lays out
        # the room. Same mkBlenderModel path as goblinModel / interceptorModel.
        droidModel = blenderLib.mkBlenderModel {
          name = "droid";
          script = "droid.py";
          animated = true;
        };
        alienModel = blenderLib.mkBlenderModel {
          name = "alien";
          script = "alien.py";
          animated = true;
        };
        # Hand-authored Quake .map for the cinematic-demo's hangar room.
        # Same mkRawAsset path as quakeMap / ootMap so kiln_map reads it via
        # rom:/maps/hangar.map at runtime. compress=0 because kiln_map_load
        # is the consumer — there is no asset_load in the path, so a
        # compressed .map would arrive still-compressed and fail to parse.
        hangarMap = assetLib.mkRawAsset {
          name = "hangar-map";
          src = ./assets/hangar.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };

        # Verifies mkQuakeMapModel through the hermetic pipeline end to end:
        # a single 6-plane cube brush, the same content
        # tools/blender-mcp/server.py's own inspect/import tools were checked
        # against before this Nix wiring was written.
        # FPS level: a larger Quake .map with multiple enemies, loaded at
        # runtime via kiln_map (same raw-asset path as ootMap / hangarMap).
        fpsMap = assetLib.mkRawAsset {
          name = "fps-level-map";
          src = ./assets/fps_level.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };
        # Per-room maps for the multi-room streaming FPS.
        fpsRoom0 = assetLib.mkRawAsset {
          name = "fps_room0";
          src = ./assets/fps_room0.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };
        fpsRoom1 = assetLib.mkRawAsset {
          name = "fps_room1";
          src = ./assets/fps_room1.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };
        fpsRoom2 = assetLib.mkRawAsset {
          name = "fps_room2";
          src = ./assets/fps_room2.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };
        # SFX for the FPS: gunfire, wall impact, enemy hit, pickup, metal impact.
        gunshotSfx = assetLib.mkSound { name = "gunshot"; src = ./assets/gunshot.wav; };
        impactSfx  = assetLib.mkSound { name = "impact";  src = ./assets/impact.wav; };
        enemyHitSfx = assetLib.mkSound { name = "enemy-hit"; src = ./assets/enemy_hit.wav; };
        pickupSfx  = assetLib.mkSound { name = "pickup";  src = ./assets/pickup.wav; };
        impactMetalSfx = assetLib.mkSound { name = "impact-metal"; src = ./assets/impact_metal.wav; };
        doorOpenSfx = assetLib.mkSound { name = "door-open"; src = ./assets/door_open.wav; };
        doorLockedSfx = assetLib.mkSound { name = "door-locked"; src = ./assets/door_locked.wav; };
        chestOpenSfx = assetLib.mkSound { name = "chest-open"; src = ./assets/chest_open.wav; };
        explosionSfx = assetLib.mkSound { name = "explosion"; src = ./assets/explosion.wav; };
        rocketFireSfx = assetLib.mkSound { name = "rocket-fire"; src = ./assets/rocket_fire.wav; };
        plasmaFireSfx = assetLib.mkSound { name = "plasma-fire"; src = ./assets/plasma_fire.wav; };
        shotgunFireSfx = assetLib.mkSound { name = "shotgun-fire"; src = ./assets/shotgun_fire.wav; };
        npcTalkSfx = assetLib.mkSound { name = "npc-talk"; src = ./assets/npc_talk.wav; };

        quakeTestModel = blenderLib.mkQuakeMapModel {
          name = "quake-test";
          src = ./assets/quake_test.map;
        };

        # Prefix stage 2: the single prefix every ROM build sees as $N64_INST.
        # streamdb-emb is already in n64InstBase; listed here only for clarity
        # (symlinkJoin dedupes by store path, so no double-mount).
        n64Inst = import ./nix/n64-inst.nix {
          inherit pkgs;
          libdragon = libdragon-sdk;
          extraLibs = [ tiny3d-sdk kiln-engine streamdb-emb ];
        };

        # The ROM builder. Projects bring a Makefile; this supplies the
        # hermetic environment. See nix/rom.nix for why.
        mkN64Rom = import ./nix/rom.nix {
          inherit pkgs toolchain n64Inst;
        };

        hello = mkN64Rom {
          name = "hello";
          src = ./examples/hello;
          romTitle = "Kiln Hello";
        };

        # The Faust bridge and the gates that enforce the report's constraints.
        faust = import ./nix/faust.nix {
          inherit pkgs toolchain;
          libdragon = n64Inst;
        };

        # A live single-precision voice, used as the worked example and as the
        # regression test for the no-libm / no-double gates.
        ks-voice = faust.mkFaustVoice {
          name = "ksvoice";
          src = ./dsp/ks.dsp;
          sampleRate = 32000;
        };

        # The same instrument taken down the OTHER path — rendered at full
        # quality on the host and VADPCM-encoded. Report §5's hybrid
        # recommendation is to bake most of the audio this way and reserve live
        # synthesis for what must be parametric, so both paths are first-class.
        ks-baked = faust.mkBakedInstrument {
          name = "ksvoice";
          src = ./dsp/ks.dsp;
          sampleRate = 32000;
          duration = 2.0;
          # gain 0.1 is not arbitrary: pm.ks runs hot, and anything above ~0.1
          # clips against the renderer's [-1,1] clamp. The clipping gate in
          # mkBakedInstrument found this — at the obvious-looking 0.8, 15% of
          # samples were clamped. Peak here is -2.8 dBFS.
          params = { freq = 220; gain = 0.1; };
          gate = { param = "gate"; on = 0.0; off = 0.02; };
        };

        # Report Stage 1 proved end to end: bake -> DFS -> RSP mixer -> ROM.
        audio = mkN64Rom {
          name = "audio";
          src = ./examples/audio;
          romTitle = "Kiln Audio";
          assets = [ ks-baked ];
          audioRate = 32000;
        };

        # Live Faust voice + baked instrument A/B comparison (report Stage 2).
        # Links the ks-voice MIPS object into the ROM and renders it
        # sample-by-sample on the VR4300, mixed with the RSP mixer output.
        live-voice = mkN64Rom {
          name = "live-voice";
          src = ./examples/live-voice;
          romTitle = "Kiln Live Voice";
          assets = [ ks-baked ];
          audioRate = 32000;
          makeFlags = [ "FAUST_VOICE=${ks-voice}/lib/ksvoice.o" ];
        };

        # 3D + 2D GUI worked example.
        engine-demo = mkN64Rom {
          name = "engine";
          src = ./examples/engine;
          romTitle = "Kiln Engine";
        };

        # Open-world streaming demo: scratch allocator, refcounted cache,
        # tile residency manager, LOD selector, two-pass renderer, and
        # (Phase F) kiln_stream + kiln_streamio pacing real kiln_asset loads
        # through kiln_cache instead of the hand-built stub loaders every
        # other kiln_tile consumer still uses.
        openworld-demo = mkN64Rom {
          name = "openworld-demo";
          src = ./examples/openworld-demo;
          romTitle = "Kiln Open World";
          assets = [ owStreamdb ];
        };

        # XM64 tracker music playback example. mkMusic converts the .xm
        # via audioconv64; the ROM plays it through libdragon's XM64 player.
        test-music = assetLib.mkMusic {
          name = "test";
          src = ./examples/music/test.xm;
        };

        # Cinematic music bed — a 15s dark-sci-fi loop synthesised in
        # examples/music/synth_loop.py (no MIDI, no soundfont, no .xm — just
        # sine + saw + noise at 32 kHz). audioconv64 turns it into VADPCM
        # .wav64 at ~250 KB; libdragon's wav64 player loops it natively. We
        # bypass the .xm/xm_tick path entirely so the BPM-0 divide-by-zero
        # family of bugs (and xm_tick's libm+float ops) is off the table.
        cine-music = assetLib.mkSound {
          name = "cine_loop";
          src = ./examples/music/cine_loop.wav;
          loop = true;
        };

        music-demo = mkN64Rom {
          name = "music";
          src = ./examples/music;
          romTitle = "Kiln Music";
          assets = [ test-music ];
          audioRate = 32000;
        };

        sc64deployer = import ./nix/tools/sc64deployer.nix {
          inherit pkgs;
          src = summercart64;
        };

        unfloader = import ./nix/tools/unfloader.nix {
          inherit pkgs;
          src = unfloader-src;
        };

        # Phase A verification: a ROM that loads one of each converted asset
        # kind — proves gltf_to_t3d, mksprite and audioconv64 actually run,
        # not just that the Nix glue around them evaluates.
        assets-demo = mkN64Rom {
          name = "assets-demo";
          src = ./examples/assets-demo;
          romTitle = "Kiln Assets";
          assets = [ assetsDemoPak demoSound ];
          audioRate = 32000;
        };

        # Phase B verification: the actor system (engine/src/kiln/kiln_actor.*)
        # with one profile per category that matters here — spawn, handle-based
        # despawn, an actor despawning itself mid-update, and the fixed
        # category draw order all exercised in one ROM.
        actors-demo = mkN64Rom {
          name = "actors-demo";
          src = ./examples/actors-demo;
          romTitle = "Kiln Actors";
        };

        # Scene/room streaming — a 2×2 grid of rooms, camera starts in room A.
        # The kiln_room module loads the room under the camera and its
        # neighbours; HUD reports current room + loaded count.
        rooms-demo = mkN64Rom {
          name = "rooms-demo";
          src = ./examples/rooms-demo;
          romTitle = "Kiln Rooms";
        };

        # Phase B completion: kiln_camera (OoT-style spring-arm follow) +
        # kiln_skel (skeletal animation, idle/swing blend) + kiln_audio
        # (footstep SFX on distance travelled) together in one ROM, driving
        # the kiln_actor player already exercised by actors-demo.
        camera-skel-demo = mkN64Rom {
          name = "camera-skel-demo";
          src = ./examples/camera-skel-demo;
          romTitle = "Kiln Camera Skel";
          assets = [ skelModel demoSound ];
          audioRate = 32000;
        };

        # Phase C verification: same three asset kinds as assets-demo, but
        # loaded from a single StreamDB container mounted at boot via
        # kiln_asset_open. Exercises kiln_asset_model (the patched
        # t3d_model_load_buf path), kiln_asset_sprite (sprite_load_buf), and
        # kiln_asset_load on a raw level blob, plus kiln_asset_count and
        # kiln_asset_find_suffix.
        streamdb-demo = mkN64Rom {
          name = "streamdb-demo";
          src = ./examples/streamdb-demo;
          romTitle = "Kiln StreamDB";
          assets = [ demoStreamdb ];
        };

        # Phase C step 1: kiln_input (deadzoned joypad wrapper with button
        # edges) + kiln_clip (swept-AABB-vs-brushes collision with iterative
        # SlideMove). One player box pushed around a 5-brush room; the box
        # slides along walls, HUD reports the last trace's fraction / normal /
        # surface. No assets, no actors — the proof stays focused on the
        # collision primitive.
        clip-demo = mkN64Rom {
          name = "clip-demo";
          src = ./examples/clip-demo;
          romTitle = "Kiln Clip";
          assets = [ demoSound stepSound ];
        };

        # Phase E: kiln_room brush auto-install + kiln_clip broadphase toggle
        # + kiln_physics HL2-style rigid bodies. One room (floor + 4 walls,
        # brushes auto-installed via kiln_room); 6 dynamic crate bodies fall,
        # stack, rest, sleep; A punts the nearest crate in a forward cone
        # (gravity-gun feel); D-pad toggles PHYS ON/OFF and BP ON/OFF; HUD
        # shows the last trace's brush count so the broadphase win is visible.
        physics-demo = mkN64Rom {
          name = "physics-demo";
          src = ./examples/physics-demo;
          romTitle = "Kiln Physics";
        };

        # Phase C step 2: kiln_dict + kiln_map. Loads assets/quake_test.map,
        # parses it into brushes + face quads, and spawns the player at the
        # info_player_start entity by reading "origin" from the KilnDict.
        map-demo = mkN64Rom {
          name = "map-demo";
          src = ./examples/map-demo;
          romTitle = "Kiln Map";
          assets = [ quakeMap ];
        };

        # The engine's own boot splash (kiln_splash.h), booted straight into.
        # kilnLogo is the same model kiln_splash_apply's camera comment is
        # tuned for — see tools/blender/kiln_logo.py.
        splash-demo = mkN64Rom {
          name = "splash-demo";
          src = ./examples/splash-demo;
          romTitle = "Kiln Splash";
          assets = [ kilnLogo ];
        };

        # Phase 4: kiln_event. A switch actor posts DOOR_OPEN with a 500 ms
        # delay; the door actor's event callback rotates it open. HUD shows
        # the queued-event count so the 500 ms gap is visible.
        event-demo = mkN64Rom {
          name = "event-demo";
          src = ./examples/event-demo;
          romTitle = "Kiln Event";
        };

        # Phase 6: the OoT + id Tech 4 integration proof. A player actor
        # (kiln_player locomotion) walks an oot_test.map room, slides via
        # kiln_clip, Z-targets enemies (kiln_target + camera TARGETING mode),
        # and emits footstep SFX through kiln_event + kiln_sound shaders.
        oot-demo = mkN64Rom {
          name = "oot-demo";
          src = ./examples/oot-demo;
          romTitle = "Kiln OoT";
          assets = [ ootMap stepSound ];
        };

        # Same integration proof, but with the retro console + profiler wired in.
        # This is the debug build of oot-demo; keep the vanilla one lean.
        oot-demo-debug = mkN64Rom {
          name = "oot-demo-debug";
          src = ./examples/oot-demo;
          romTitle = "Kiln OoT Debug";
          assets = [ ootMap stepSound ];
          debugConsole = true;
        };

        # The on-screen retro debug console. Built with `debugConsole = true`
        # so KILN_DEBUG=1 reaches the example's main.c (gating the
        # kiln_console_* calls). The console module itself is always in
        # libkiln.a; the flag only controls whether the example wires it up.
        # Toggle in-rom by holding Start and pressing C-Up → C-Left →
        # C-Down → C-Right (counter-clockwise around the C cluster).
        debug-demo = mkN64Rom {
          name = "debug-demo";
          src = ./examples/debug-demo;
          romTitle = "Kiln Debug";
          debugConsole = true;
        };

        # The single-screen showcase: title + 3-mode flight + engine streaks +
        # credit HUD. Loads the hand-authored Interceptor starfighter through
        # the same mkBlenderModel path tools/blender/interceptor.py documents.
        interceptor-demo = mkN64Rom {
          name = "interceptor-demo";
          src = ./examples/interceptor-demo;
          romTitle = "Kiln Interceptor";
          assets = [ interceptorModel demoSound test-music ];
          audioRate = 32000;
        };

        # texanim-demo: exercises kiln_texanim (UV scroll, flipbook, palette,
        # offscreen) and kiln_vanim (procedural deform, morph blending, RSP
        # vertex FX). All geometry is hand-built — no asset pipeline needed.
        texanim-demo = mkN64Rom {
          name = "texanim-demo";
          src = ./examples/texanim-demo;
          romTitle = "Kiln TexAnim";
        };

        # Cinematic-demo: a 60-second single-shot scene of the Interceptor in
        # its hangar with the goblin captain walking the perimeter, droids
        # servicing the ship, and aliens approaching from the back. Exercises
        # every engine subsystem in one ROM — input, player, clip, target,
        # surface, sound, event, dict, map, room, camera (CUTSCENE mode), skel
        # (goblin walk-cycle), audio (music + SFX). Assets:
        #   interceptorModel / goblinModel / droidModel / alienModel — the cast
        #   hangarMap           — the Quake-format .map room
        #   demoSound / stepSound — SFX (engine whoosh, footsteps)
        #   cine-music          — VADPCM .wav64 loop (15s dark-sci-fi bed)
        cinematic-demo = mkN64Rom {
          name = "cinematic-demo";
          src = ./examples/cinematic-demo;
          romTitle = "Kiln Cinematic";
          assets = [
            interceptorModel goblinModel droidModel alienModel
            hangarMap demoSound stepSound cine-music
          ];
          audioRate = 32000;
        };

        # A minimal playable first-person shooter. First-person camera
        # (kiln_fpscam), hitscan weapon (kiln_weapon), enemy actors that chase
        # the player, HUD with crosshair + health + ammo. The FPS level is a
        # Quake .map loaded at runtime via kiln_map.
        fps = mkN64Rom {
          name = "fps";
          src = ./examples/fps;
          romTitle = "Kiln FPS";
          assets = [ fpsRoom0 fpsRoom1 fpsRoom2 gunshotSfx impactSfx enemyHitSfx pickupSfx impactMetalSfx doorOpenSfx doorLockedSfx chestOpenSfx explosionSfx rocketFireSfx plasmaFireSfx shotgunFireSfx npcTalkSfx ];
          audioRate = 32000;
        };

        # ── Bass synth ────────────────────────────────────────────────────
        # 4-controller collaborative bass ROM. 12 dual-layer wavetables
        # (4 engines x {body_bright, body_dark, sub}) generated by
        # tools/gen_bass_wav.py, baked as looping VADPCM wav64s and
        # pitched live by the RSP mixer. Each held note consumes 2 mixer
        # channels (body + sub) → 6-voice polyphony across 12 channels.
        bassWav = engine: layer: assetLib.mkSound {
          name = "bass_${engine}_${layer}";
          src = ./assets/bass_wav/${engine}_${layer}.wav;
          loop = true;
          mono = true;
        };
        bassWavFlat = let
          layers = [ "body_bright" "body_dark" "sub" ];
          engines = [ "heavy" "sub" "growl" "industrial" ];
        in pkgs.lib.concatMap (e: map (l: bassWav e l) layers) engines;

        bass-synth = mkN64Rom {
          name = "bass-synth";
          src = ./examples/bass-synth;
          romTitle = "Kiln Bass Synth";
          assets = bassWavFlat;
          audioRate = 32000;
        };

        # ── Forge ────────────────────────────────────────────────────────
        # A standalone tool ROM: a voxel level/cinematic editor that runs on the
        # console, so a level is judged where it will be played rather than two
        # tool hops away. See the plan in
        # /home/asher/.claude/plans/minecraft-like-game-that-fluttering-minsky.md
        #
        # `forge-selftest` comes FIRST and is deliberately its own ROM. Forge's
        # whole iteration loop rests on one assumption — that a ROM can write
        # files to the SD card of an ED64 Plus, which is a clone board libcart
        # names but nobody here has proved. This probe answers that in one boot,
        # exercising kiln_store's real write path rather than a copy of it.
        #
        # saveType is DELIBERATELY not eeprom/sram here: the probe reports
        # whether the SD path works, and a declared save type would have the
        # EverDrive OS offering to flush a save that the probe is not using.
        # The SRAM fallback gets its own variant below when it is needed.
        forge-selftest = mkN64Rom {
          name = "forge-selftest";
          src = ./Forge/selftest;
          romTitle = "Kiln Forge Probe";
        };

        # The editor itself. No `assets` and no saveType: the level lives on the
        # SD card, not in the ROM and not in a save chip, which is exactly the
        # property that makes editing cost no rebuild. A DFS-seeded variant for
        # emulator inspection lands with the mode work that needs it.
        forge = mkN64Rom {
          name = "forge";
          src = ./Forge;
          romTitle = "Kiln Forge";
        };

        # Forge with a level baked into the ROM. Its only purpose is to make the
        # LOAD path verifiable without a cart: `./dev shot forge-dfs` boots it,
        # kiln_store falls through SD and the save chip to read-only `rom:/`, and
        # the level either appears or the HUD says which step refused.
        forge-dfs = mkN64Rom {
          name = "forge-dfs";
          src = ./Forge;
          romTitle = "Kiln Forge DFS";
          assets = [ forgeSeedLevel ];
        };

        # `nix build .#forge-<mode>` boots straight into one mode over the baked
        # level. `./dev shot` has no input path at all by design and `./dev
        # drive`'s uinput chain is fragile, so a mode reached only by a chord is
        # a mode that can only be verified by hand — which for WALK (the one
        # whose entire purpose is standing in the level) and CAM (whose whole
        # output is a curve you have to SEE) is most of the value.
        # No `assets`, deliberately: with nothing to load these fall through to
        # forge_io_seed's demo content, which is a room WITH a spawn and a
        # three-key shot. The baked level is geometry only — frg.py imports a
        # `.map`, and a `.map` has no camera path — so a CAM capture over it
        # correctly reported `ERR nokeys` and showed an empty timeline.
        #
        # The alternative was teaching frg.py to invent a camera path on import,
        # which would put content nobody authored into every level anyone
        # imported. So the demo content has exactly ONE author (forge_io_seed),
        # and `.#forge-dfs` still covers the load path it is there to cover.
        forgeModes = [ "WALK" "PAINT" "ENT" "LIGHT" "CAM" ];
        forgeModeRoms = pkgs.lib.listToAttrs (map
          (m: pkgs.lib.nameValuePair "forge-${pkgs.lib.toLower m}" (mkN64Rom {
            name = "forge-${pkgs.lib.toLower m}";
            src = ./Forge;
            romTitle = "Kiln Forge ${m}";
            makeFlags = [ "FORGE_MODE=${m}" ];
          }))
          forgeModes);

        # The same probe with a save chip declared, which is the ONLY way to
        # exercise kiln_store's SRAM fallback: `sram_detect()` round-trips a word
        # through the cart, so with no save type in the ROM header there is
        # nothing there to detect and the fallback correctly refuses itself.
        #
        # Two ROMs rather than one flag because this is a gate on a fallback and
        # a gate should be verified to fire in BOTH directions — the plain build
        # must report "no writable backend" under an emulator, this one must
        # round-trip 8 KB through SRAM. Testing only the arm that passes is how
        # you ship a fallback that was never once exercised.
        forge-selftest-sram = mkN64Rom {
          name = "forge-selftest-sram";
          src = ./Forge/selftest;
          romTitle = "Kiln Forge Probe SRAM";
          saveType = "sram256k";
        };

        # Phase 1 verification: kiln_rng + kiln_dice + kiln_board + kiln_turn
        # end-to-end. 4 tokens, 5 rounds, a 10-node branching path, an
        # auto-advancing state machine. No assets — the proof is the
        # topology and the turn transitions, drawn as a 2D HUD schematic.
        board-demo = mkN64Rom {
          name = "board-demo";
          src = ./examples/board-demo;
          romTitle = "Kiln Board";
        };

        # ── The Kiln boot splash ──────────────────────────────────────────
        # A parody of the Nintendo 64's boot, and a publisher mark rather
        # than any one game's title screen — which is why the runtime half
        # is in the engine (engine/src/kiln/kiln_splash.h) and only the two
        # assets live here. Any game built on this engine can adopt it with
        # four calls.
        kilnLogo = blenderLib.mkBlenderModel {
          name = "kiln_logo";
          script = "kiln_logo.py";
          # baseScale is MODEL UNITS PER BLENDER UNIT, not a "keep my units"
          # switch — flake.nix's own demoModel note says so ("a 1x1x1 Blender
          # unit cube; the default baseScale of 64 turns that into a 64-unit
          # cube"). At 1, this 3.1-unit-wide wordmark became 3 integer units
          # and collapsed into an unreadable grey slab on screen. Tiny3D
          # stores vertices as integers; sub-unit detail simply does not
          # survive. 64 keeps it consistent with every other model here, and
          # kiln_splash's camera is placed in the same units.
          baseScale = 64;
        };

        # The jingle. Its chord resolves 1.25 s in, which kiln_splash.c
        # times the logo's assembly and the screen flash to meet — picture
        # can be nudged a frame at runtime, audio cannot, so the sound is
        # the master here.
        kilnJingle = faust.mkBakedInstrument {
          name = "kilnjingle";
          src = ./dsp/kiln_jingle.dsp;
          sampleRate = 32000;
          duration = 4.0;
          params = { gain = 0.5; };
          # loop = true even though the jingle plays ONCE.
          #
          # A non-looping wav64 reaching its end crashed the mixer:
          # libdragon asserts "samplebuffer_get: no reader to extend" a
          # couple of seconds later, because a finished one-shot is still in
          # mixer_poll's rotation with nothing left to read. The looping
          # ambience bed never showed it, and that difference is the whole
          # clue. A looping sample simply never reaches that state.
          #
          # kiln_splash stops the channel at 3.8 s and the sample is 4.0 s,
          # so it is stopped before it would ever wrap — the loop flag costs
          # nothing audible and removes the end-of-sample path entirely.
          loop = true;
        };

        # A Forge level baked into a ROM, so the LOAD path can be verified under
        # an emulator — which has no SD card, so `.#forge`'s normal storage
        # backend is unreachable there and its read path would otherwise only
        # ever be exercised on hardware. Generated from a committed .map by the
        # same host tool `./dev forge-push` uses, so this is not a special export:
        # it is the file the card would hold.
        forgeSeedFrg = pkgs.runCommand "forge-seed-frg"
          { nativeBuildInputs = [ pkgs.python3 ]; }
          ''
            mkdir -p $out
            python3 ${./tools/forge/frg.py} frommap ${./assets/oot_test.map} \
                    $out/LEVEL.frg
          '';

        # compress = 0 because kiln_store reads this with a plain fopen, not
        # asset_fopen: an mkasset-compressed payload would come back as its
        # container bytes and fail the CRC, which is a confusing way to discover
        # a compression setting.
        forgeSeedLevel = assetLib.mkRawAsset {
          name = "LEVEL";
          src = "${forgeSeedFrg}/LEVEL.frg";
          dest = "forge";
          extension = "frg";
          compress = 0;
        };

        # A NixOS-in-Docker image for collaborators: real Nix (so `nix
        # build`/`nix develop`/`./dev` work inside it against a cloned
        # checkout of this repo) plus the Claude Code CLI and Tailscale, for
        # private delivery/updates over a tailnet. See nix/dev-image.nix for
        # what it deliberately does and does not contain.
        dev-image = nixos-generators.nixosGenerate {
          inherit system;
          format = "docker";
          modules = [ ./nix/dev-image.nix ];
          specialArgs = { inherit claude-code-nix; };
        };
      in
      {
        packages = {
          # ── playable host builds ───────────────────────────────────
          # The same examples/<x>/main.c the ROM builds, compiled with
          # -Dmain=kiln_game_main and linked against plat/shell. engine-demo
          # first because it is the report's own benchmark for a working port
          # — a lit spinning cube plus a HUD, which on hardware runs at 59.8
          # fps and here exercises the 3D pass, the seam and the 2D pass.
          pc-engine-demo = hostNative.mkGame {
            pname = "kiln-engine-demo";
            sources = [ ./examples/engine/main.c ];
            meta.description = "engine-demo, playable on this machine";
          };
          web-engine-demo = hostWasm.mkGame {
            pname = "kiln-engine-demo";
            sources = [ ./examples/engine/main.c ];
            meta.description = "engine-demo, playable in a browser";
          };

          # The host tier's own artefacts, per architecture.
          host-arch-native  = archProof hostNative;
          host-arch-wasm32  = archProof hostWasm;
          host-arch-aarch64 = archProof hostTargets.targets.aarch64;
          host-arch-riscv64 = archProof hostTargets.targets.riscv64;
          host-libs         = hostNative.engine;
          host-backend      = hostNative.backend;

          inherit toolchain hello audio live-voice music-demo engine-demo ks-voice ks-baked sc64deployer unfloader n64Inst assets-demo actors-demo rooms-demo streamdb-demo camera-skel-demo clip-demo physics-demo map-demo splash-demo event-demo oot-demo oot-demo-debug debug-demo interceptor-demo cinematic-demo texanim-demo fps bass-synth openworld-demo board-demo forge forge-dfs forge-selftest forge-selftest-sram;
          engine = kiln-engine;
          host-math = hostMath;
          streamdb = streamdb-emb;
          inherit textures;
          inherit dev-image;
        }
        // forgeModeRoms
        # `nix build .#model-torus` converts one model on its own, which is the
        # fast loop when a shape comes out wrong: each derivation keeps its
        # intermediate glTF in share/gltf/, so geometry problems can be told
        # apart from material problems without rebuilding anything.
        // pkgs.lib.mapAttrs' (n: v: pkgs.lib.nameValuePair "model-${n}" v)
          testModels
        // {
          # Exposed so ./dev shot can resolve the emulator binary directly.
          ares-bin = pkgs.ares;
          libdragon = libdragon-sdk;
          tiny3d = tiny3d-sdk;
          model-goblin = goblinModel;
          model-interceptor = interceptorModel;
          model-droid = droidModel;
          model-alien = alienModel;
          model-quake-test = quakeTestModel;
          model-kiln-logo = kilnLogo;
          # The splash's two assets, exposed as a pair. kiln_splash is
          # engine-level — a PUBLISHER mark, not any one game's — so a
          # downstream game adopts it by putting these two in its own ROM's
          # `assets` list, not by rebuilding them. Both are optional and the
          # splash's timing is identical without them; see kiln_splash.h.
          audio-kiln-jingle = kilnJingle;
          default = hello;
        };

        # Exposed so downstream flakes (the SSHitunneller! N64 port, and the
        # two games split out of this tree — PetaByte Madness and Ganja
        # Goblin) can build ROMs and voices against this pinned toolchain
        # without vendoring it.
        #
        # The rule for what belongs here: a builder a DOWNSTREAM repo needs to
        # ship its own content. Splitting the games out is what turned that
        # from a hypothetical into a hard requirement — PetaByte Madness
        # authors geometry with mkBlenderModel, ships a CI4 veil palette pair
        # with mkVeilTexture, an XM64 score with mkMidiMusic, an MPEG1 title
        # card with mkVideo and the sea's foam sprite with mkTextures, and
        # none of the five were reachable from outside this file. A builder
        # that exists but is not exposed is a builder a game has to
        # reimplement, which is the drift the single-source-of-truth rule in
        # engine/modules.mk exists to prevent, one level up.
        lib = {
          inherit mkN64Rom;
          inherit (faust) mkFaustVoice mkBakedInstrument mkOfflineRenderer;
          inherit (assetLib) mkModel mkSprite mkFont mkSound mkMusic mkRawAsset
                             mkStreamdb mkAssetPak mkTextures mkVideo
                             mkMidiMusic mkVeilTexture;
          inherit (blenderLib) mkBlenderModel mkQuakeMapModel mkGodotSceneModel;
        };

        checks = {
          toolchain = import ./nix/checks/toolchain.nix { inherit pkgs toolchain; };
          rom-hello = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = hello;
            name = "hello";
          };
          rom-audio = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = audio;
            name = "audio";
          };
          rom-engine = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = engine-demo;
            name = "engine";
          };
          rom-assets-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = assets-demo;
            name = "assets-demo";
          };
          rom-actors-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = actors-demo;
            name = "actors-demo";
          };
          rom-rooms-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = rooms-demo;
            name = "rooms-demo";
          };
          rom-music-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = music-demo;
            name = "music";
          };
          rom-live-voice = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = live-voice;
            name = "live-voice";
          };
          rom-streamdb-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = streamdb-demo;
            name = "streamdb-demo";
          };
          rom-clip-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = clip-demo;
            name = "clip-demo";
          };
          rom-physics-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = physics-demo;
            name = "physics-demo";
          };
          rom-map-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = map-demo;
            name = "map-demo";
          };
          rom-splash-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = splash-demo;
            name = "splash-demo";
          };
          rom-event-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = event-demo;
            name = "event-demo";
          };
          rom-oot-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = oot-demo;
            name = "oot-demo";
          };
          rom-oot-demo-debug = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = oot-demo-debug;
            name = "oot-demo-debug";
          };
          rom-debug-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = debug-demo;
            name = "debug-demo";
          };
          rom-interceptor-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = interceptor-demo;
            name = "interceptor-demo";
          };
          rom-cinematic-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = cinematic-demo;
            name = "cinematic-demo";
          };
          rom-texanim-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = texanim-demo;
            name = "texanim-demo";
          };
          rom-fps = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = fps;
            name = "fps";
          };
          rom-bass-synth = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = bass-synth;
            name = "bass-synth";
          };
          rom-board-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = board-demo;
            name = "board-demo";
          };
          # rom.nix globs for *.z64 rather than taking a filename, which is
          # what makes this work for Forge: the Makefile emits `forge.z64` no
          # matter which flake attribute built it.
          rom-forge = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = forge;
            name = "forge";
          };
          rom-forge-selftest = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = forge-selftest;
            name = "forge-selftest";
          };
          kiln-asset = import ./nix/checks/kiln-asset.nix {
            inherit pkgs;
            streamdbSrc = streamdb;
            embeddedSrc = ./streamdb-embedded;
            engineSrc = ./engine;
          };
          streamdb = import ./nix/checks/streamdb.nix {
            inherit pkgs;
            streamdbSrc = streamdb;
            embeddedSrc = ./streamdb-embedded;
          };
          assets = import ./nix/checks/assets.nix {
            inherit pkgs;
            n64Inst = n64InstBase;
          };
          mapmaker-roundtrip = import ./nix/checks/mapmaker-roundtrip.nix {
            inherit pkgs;
          };
          # The host build of libdragon's fast math must compute what the
          # VR4300 computes, down to the tie-breaking rule.
          kiln-hostmath = import ./nix/checks/kiln-hostmath.nix {
            inherit pkgs hostMath;
          };
          # The rename is an invariant, not a state: reintroducing the old
          # name for this engine fails the build. See the check's own header.
          kiln-names = import ./nix/checks/kiln-names.nix {
            inherit pkgs;
            repo = ./.;
          };
          # Forge's own content round trip: the .FRG container, the host mirror
          # of kiln_voxel_boxes, and the .map emitter against the STRICT reader.
          # Held to mapmaker-roundtrip's standard by the same method, because the
          # two .map emitters have to agree about the same brush.
          forge-roundtrip = import ./nix/checks/forge-roundtrip.nix {
            inherit pkgs;
          };
          # The bpy-free geometry builder tests. Gated here because they are
          # the fastest feedback loop in the modelling pipeline and were, until
          # this entry existed, run by nobody — see the check's own header for
          # what that cost.
          blender-tests = import ./nix/checks/blender-tests.nix {
            inherit pkgs;
          };
          # The engine's pure-logic modules, compiled natively against
          # nix/checks/stub/ and asserted on. Found two real bugs on its first
          # run — see the check's own header.
          # engine/modules.mk's host tier must be exactly the set that
          # compiles natively — checked in both directions from one run.
          # The host font is libdragon's own, extracted; the header must be
          # current and the font must still be monospaced.
          kiln-font = import ./nix/checks/kiln-font.nix {
            inherit pkgs;
            libdragonSrc = libdragon;
            platHost = ./plat/host;
            fontTool = ./tools/font_extract.py;
          };
          # The 2D pass, rendered by the real kiln_gui.c through the host
          # software rasteriser and diffed against a committed capture. The
          # frame gate CLAUDE.md says needs a Wayland session — it does not,
          # once the renderer is software.
          kiln-gui = import ./nix/checks/kiln-gui.nix {
            inherit pkgs;
            target = hostNative;
          };
          # The same check body, the same two reference files, compiled to
          # wasm32 and run under node. Byte-identical output from a 32-bit
          # pointer target with a different libm is the actual claim that the
          # host tier is architecture-independent — a per-architecture
          # reference would only prove each one agrees with itself.
          kiln-gui-wasm32 = import ./nix/checks/kiln-gui.nix {
            inherit pkgs;
            target = hostWasm;
          };
          # The 3D pass is where an architecture actually gets to disagree:
          # every one of these renders through host_t3d.c's float edge
          # functions, and -ffp-contract=off (nix/host.nix) is what stops a
          # target with an FMA from fusing them and moving every triangle
          # edge by a ULP. These four hold wasm32 to the x86_64 references.
          kiln-scene-wasm32 = import ./nix/checks/kiln-scene.nix {
            inherit pkgs; target = hostWasm;
          };
          kiln-model-wasm32 = import ./nix/checks/kiln-model.nix {
            inherit pkgs n64Inst; target = hostWasm;
            cubeGltf = ./assets/cube.gltf;
          };
          kiln-map-wasm32 = import ./nix/checks/kiln-map.nix {
            inherit pkgs; target = hostWasm;
            mapAsset = ./assets/quake_test.map;
          };
          kiln-splash-wasm32 = import ./nix/checks/kiln-splash.nix {
            inherit pkgs n64Inst kilnLogo; target = hostWasm;
          };
          kiln-voxmesh-wasm32 = import ./nix/checks/kiln-voxmesh.nix {
            inherit pkgs; target = hostWasm;
          };
          # kiln_widget's screens, rendered through the same host backend and
          # diffed against their committed captures.
          kiln-widget = import ./nix/checks/kiln-widget.nix {
            inherit pkgs; target = hostNative;
            uipreviewSrc = ./tools/uipreview;
          };
          # The whole frame bracket — 3D pass, the seam, 2D pass — rendered
          # by the real kiln_engine.c and kiln_gui.c on the host.
          kiln-scene = import ./nix/checks/kiln-scene.nix {
            inherit pkgs; target = hostNative;
          };
          # A real voxel mesh rendered with two combiners, which is how the
          # atlas-never-sampled defect became visible instead of arguable.
          kiln-voxmesh = import ./nix/checks/kiln-voxmesh.nix {
            inherit pkgs; target = hostNative;
          };
          # A real .t3dm, converted by the same gltf_to_t3d the ROM uses,
          # parsed and rendered by the host reader.
          kiln-model = import ./nix/checks/kiln-model.nix {
            inherit pkgs n64Inst; target = hostNative;
            cubeGltf = ./assets/cube.gltf;
          };
          # The engine's real boot splash — kiln + flame + lit publisher
          # line — rendered by the actual kiln_splash.c, not a stand-in.
          kiln-splash = import ./nix/checks/kiln-splash.nix {
            inherit pkgs n64Inst kilnLogo; target = hostNative;
          };
          # A real Quake .map, loaded off the host VFS and rendered. Found
          # two defects in kiln_map and pins both — see the check's header.
          kiln-map = import ./nix/checks/kiln-map.nix {
            inherit pkgs; target = hostNative;
            mapAsset = ./assets/quake_test.map;
          };
          # The launcher runs the real examples/engine/main.c game loop for
          # ninety frames under SDL's dummy drivers. See the check's header
          # for why it is statistics and not a golden image.
          kiln-shell = import ./nix/checks/kiln-shell.nix {
            inherit pkgs;
            game = self.packages.${system}.pc-engine-demo;
          };
          # A real audioconv64 .wav64 decodes to PCM with energy in it, mixes,
          # and pans the right way round. See the check's header on why RMS
          # and not sample count.
          kiln-wav64 = import ./nix/checks/kiln-wav64.nix {
            inherit pkgs; target = hostNative; sound = demoSound;
          };
          kiln-wav64-wasm32 = import ./nix/checks/kiln-wav64.nix {
            inherit pkgs; target = hostWasm; sound = demoSound;
          };
          # The browser launcher itself — the canvas blit and the ASYNCIFY
          # game loop — run under node against a recording DOM stub. See the
          # check's header for what this can and cannot prove.
          kiln-web = import ./nix/checks/kiln-web.nix {
            inherit pkgs;
            target = hostTargets.targets.wasm32-node.mkGame {
              pname = "kiln-engine-demo";
              sources = [ ./examples/engine/main.c ];
            };
            domStub = ./nix/checks/kiln-web-dom.js;
          };
          kiln-parity = import ./nix/checks/kiln-parity.nix {
            inherit pkgs hostMath;
            engineSrc = ./engine;
            platHost = ./plat/host;
            streamdbInc = "${streamdb-emb}/mips64-elf/include";
          };
          kiln-logic = import ./nix/checks/kiln-logic.nix {
            inherit pkgs hostMath;
            engineSrc = ./engine;
            platHost = ./plat/host;
          };
          inherit hello audio live-voice music-demo engine-demo ks-voice ks-baked assets-demo actors-demo rooms-demo streamdb-demo clip-demo physics-demo map-demo splash-demo event-demo oot-demo oot-demo-debug debug-demo interceptor-demo cinematic-demo texanim-demo fps bass-synth board-demo forge forge-dfs forge-selftest forge-selftest-sram;
        }
        # The mode-jump ROMs are gated too. They are the only way each of PAINT,
        # ENT, LIGHT, CAM and WALK gets built at all — a mode reachable only by a
        # chord is a mode nothing compiles unless something asks for it, and a
        # per-mode -D flag is exactly the sort of thing that rots unnoticed.
        // forgeModeRoms;

        apps = {
          # Iterate here. Per report §7, Ares is the reference emulator for
          # homebrew — but do NOT trust any emulator for audio work: the AI
          # clock-divider rate, RDRAM latency contention, and denormal/FPU
          # behaviour are exactly what emulators get subtly wrong.
          ares = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-ares" ''
              exec ${pkgs.ares}/bin/ares "$@"
            '');
          };
          # Cycle-accurate cross-check. Too slow for iteration; useful for A/B.
          cen64 = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-cen64" ''
              exec ${pkgs.cen64}/bin/cen64 "$@"
            '');
          };
          # The hardware path: upload, debugf stdio, IS-Viewer64, and the AUX
          # PC<->N64 channel. Needs nixosModules.n64-flashcart for USB perms.
          sc64 = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-sc64" ''
              exec ${sc64deployer}/bin/sc64deployer "$@"
            '');
          };
          unfloader = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-unfloader" ''
              exec ${unfloader}/bin/unfloader "$@"
            '');
          };
          dev = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-dev" ''
              exec ${pkgs.bash}/bin/bash "''${KILN_REPO:-$PWD}/dev" "$@"
            '');
          };
          # three.js .map maker (tools/mapmaker/). A dev-only web app run
          # outside the hermetic build — same authoring/outside-build vs.
          # consume/inside-build split as tools/blender-mcp/. Exports canonical
          # .map text the existing mkQuakeMapModel + kiln_map.c pipeline already
          # consumes; validate via ./dev map-validate.
          mapmaker = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-mapmaker" ''
              cd "''${KILN_REPO:-$PWD}/tools/mapmaker"
              exec ${pkgs.python3Minimal}/bin/python3 -m http.server 8000
            '');
          };
          # The animation editor. Same shape as mapmaker: a static page of
          # ES modules served by python's http.server, three.js vendored
          # once and shared between the two tools. `stage` first, because
          # the editor loads the real pipeline's own .gltf intermediate
          # rather than a special export — see tools/poser/stage.sh.
          poser = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-poser" ''
              cd "''${KILN_REPO:-$PWD}"
              if [ ! -f tools/poser/data/dank.gltf ]; then
                echo "staging models for the poser (first run)…"
                ./tools/poser/stage.sh dank
              fi
              cd tools/poser
              echo "poser on http://localhost:8001"
              exec ${pkgs.python3Minimal}/bin/python3 -m http.server 8001
            '');
          };
          poser-verify = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-poser-verify" ''
              exec ${pkgs.python3Minimal}/bin/python3 \
                "''${KILN_REPO:-$PWD}/tools/poser/verify.py" "$@"
            '');
          };
          map-validate = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-map-validate" ''
              exec ${pkgs.python3Minimal}/bin/python3 "''${KILN_REPO:-$PWD}/tools/mapmaker/validate.py" "$@"
            '');
          };
          # tools/blender-mcp/'s MCP server. The `mcp` package comes from
          # THIS flake's own pinned nixpkgs (flake.lock), not the
          # `nix shell --impure --expr 'import <nixpkgs> {}'` the README used
          # to document — that resolved against whatever channel NIX_PATH
          # happened to point at, unpinned and unaudited, the one thing this
          # repo's whole toolchain strategy otherwise refuses to accept (see
          # CLAUDE.md's "Hard-won facts"). server.py itself runs from the
          # live checkout (not copied into the store): it locates its sibling
          # tools/blender/{quake_map,godot_scene}.py via
          # `Path(__file__).resolve().parents[2]`, which only resolves
          # correctly against a real working tree — same KILN_REPO convention
          # as `dev`/`mapmaker`/`poser` above.
          blender-mcp = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-blender-mcp" ''
              exec ${pkgs.python3.withPackages (ps: [ ps.mcp ])}/bin/python3 \
                "''${KILN_REPO:-$PWD}/tools/blender-mcp/server.py" "$@"
            '');
          };
        };

        devShells.default = pkgs.mkShell {
          packages = [
            toolchain
            n64Inst
            pkgs.gnumake
            pkgs.faust
            pkgs.ares
            pkgs.pkg-config
            # The capture loop's python: evdev drives tools/n64-input.py's
            # uinput gamepad (there is no way to reach a screen behind
            # "press start" without it), and pillow is what turns a
            # screenshot into the pixel statistics ./dev shot reports —
            # CLAUDE.md's own warning is that eyeballing the PNG has
            # already cost this project real time.
            (pkgs.python3.withPackages (ps: [ ps.evdev ps.pillow ]))
          ];

          # Bare metal: see nix/libdragon.nix.
          hardeningDisable = [ "all" ];

          N64_INST = n64Inst;
          N64_GCCPREFIX = toolchain;

          shellHook = ''
            echo "═══════════════════════════════════════════"
            echo " Kiln — N64 / ModRetro M64 development shell"
            echo " gcc        ${toolchain.passthru.version} (mips64-elf-)"
            echo " N64_INST   $N64_INST"
            echo " make · nix build .#hello · nix run .#ares -- <rom.z64>"
            echo "═══════════════════════════════════════════"
          '';
        };
      }) // {
      # ── NixOS modules (system-independent) ────────────────────────────
      nixosModules.n64-flashcart = import ./nix/udev.nix;
    };
}
