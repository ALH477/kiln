# Vendored three.js

These files are the runtime of the Kiln map maker — a deliberately-outside-`nix
build` interactive tool (see `tools/blender-mcp/`'s module docstring for the
reasoning that pattern applies here too). They are NOT a build input to any
ROM; nothing in `nix/` reads from this directory.

## Pinned version

three.js **r160** (npm `three@0.160.0`), all three files from the same release
so the addon APIs match the core. Bumping three.js means re-downloading all
three from the same tag and re-checking the editor in a browser.

## Source URLs + SHA256 (recorded at fetch time, 2026-08-03)

| File | URL | SHA256 |
| --- | --- | --- |
| `three.module.min.js` | `https://unpkg.com/three@0.160.0/build/three.module.min.js` | `3e690ac7d180b0aadf0891bea39eec643e29e2d3e75c99b18689518665f69ba6` |
| `addons/controls/OrbitControls.js` | `https://unpkg.com/three@0.160.0/examples/jsm/controls/OrbitControls.js` | `5a44a9e86a2a0fb11933eed69bc2cd33c76a496854c1aed6ed776efa87d7b064` |
| `addons/controls/TransformControls.js` | `https://unpkg.com/three@0.160.0/examples/jsm/controls/TransformControls.js` | `487861c18e017a6c69cc302827c6f506993f05dafce220da0bad095d6fe5808d` |
| `addons/renderers/CSS2DRenderer.js` | `https://unpkg.com/three@0.160.0/examples/jsm/renderers/CSS2DRenderer.js` | `a4f0f79184c043f6b9d2654d8ba051e49a7d631d34e8f437c1804798a68c379f` |

To re-fetch (e.g. for an airgapped verify), run from this directory:

```sh
for f in \
  build/three.module.min.js:three.module.min.js \
  examples/jsm/controls/OrbitControls.js:addons/controls/OrbitControls.js \
  examples/jsm/controls/TransformControls.js:addons/controls/TransformControls.js \
  examples/jsm/renderers/CSS2DRenderer.js:addons/renderers/CSS2DRenderer.js
do
  src=${f%%:*}; dst=${f##*:}
  curl -fsSL -o "$dst" "https://unpkg.com/three@0.160.0/$src"
done
sha256sum three.module.min.js addons/controls/OrbitControls.js \
          addons/controls/TransformControls.js addons/renderers/CSS2DRenderer.js
```

The Nix build does not hash these (they're tree paths, not fixed-output
derivations), so the SHA256 above is documentation, not enforcement. That's
acceptable for a dev-only tool that never touches a ROM.

## License

`LICENSE.three` carries the MIT license three.js is distributed under. The
license notice must travel with the code; do not delete it when bumping the
version.

## VENDORED_WINDING note (read before touching the editor's exporter)

`src/mapio.js`'s `aabbFaces()` emits the six faces of every brush in a
specific corner order, verified against `assets/quake_test.map:4-9` (the
file `mkQuakeMapModel` round-trips through Blender, so `quake_map.py`'s
`brush_to_faces` CSG accepts it). The engine (`kiln_map.c:109-114`) computes
the AABB as componentwise min/max of plane points and is
winding-independent, so any order would *parse* fine on console. But
`tools/mapmaker/validate.py` reuses `tools/blender/quake_map.py`'s
`brush_to_faces` CSG, and that uses `plane_normal_dist`
(`tools/blender/quake_map.py:210-222`): `cross(p3-p1, p2-p1)` — the Quake
winding convention. Emitting faces in the wrong winding makes the CSG
inside-out and `validate.py` reports zero surviving faces even though the
engine is happy.

`assets/oot_test.map` is **inside-out** relative to this convention and
would fail `validate.py` — it works on console only because the engine's
AABB reduction ignores winding. The editor's emit always uses the canonical
winding, so an imported-then-exported `oot_test.map` round-trips cleanly
through `validate.py` even though the source file does not.