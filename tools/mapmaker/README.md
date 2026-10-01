<!-- SPDX-License-Identifier: MIT -->
# mapmaker — Kiln's level tools

Two front ends, one format, one implementation of that format.

| you have | use | command |
|---|---|---|
| a browser and a mouse | the three.js editor | `./dev mapmaker` → http://localhost:8000/mapmaker/ (or `./dev studio`, which saves in place) |
| a shell (a script, a test, an agent) | JSON in, `.map` out | `./dev map-emit spec.json out.map` |
| a `.map` you want to read | `.map` out, JSON in | `./dev map-dump level.map` |
| a `.map` you want to check | the CSG + the limits | `./dev map-validate level.map [--json]` |
| a `.map` you want to *see* | a PNG, no ROM, no emulator | `./dev map-render level.map out.png` |
| a `.map` that fails the CSG | repair the winding | `./dev map-canon level.map` |
| Redot (or Godot), and a mouse | the editor's own scene format | `./dev map-from-tscn level.tscn out.map` |
| a `.map` you want to edit in Redot | the same, backwards | `./dev map-to-tscn level.map out.tscn` |

```
$ ./dev map-emit  <(./dev map-dump assets/oot_test.map -)  /tmp/x.map
$ ./dev map-validate /tmp/x.map --json | jq .problems
$ ./dev map-render   /tmp/x.map /tmp/x.png     # then look at the PNG
```

## Why the winding is the thing to know

A Quake brush is six planes, each given as three points. Their ORDER decides
which way the plane faces: `tools/blender/quake_map.py` takes the outward normal
as `cross(p3-p1, p2-p1)`. Get it backwards and the six half-spaces intersect in
nothing, so the brush yields **zero** polygons.

That failure is silent in the only place anyone looks. `engine/src/kiln/kiln_map.c`
reduces a brush to the componentwise min/max of its plane points, which is
indifferent to winding — so an inside-out level loads on console, collides
correctly, and plays. It only vanishes when it reaches Blender.

Six of this repo's seven committed `.map` files were inside-out. `map-canon`
fixed them; `./dev map-validate` now names the cause when it happens again.

**So: do not hand-write `.map` text.** `map-emit` takes JSON and cannot emit the
wrong winding, because it does not have a copy of the table to get wrong.

## Files

| file | what |
|---|---|
| `index.html`, `src/main.js` | the browser editor: scene, gizmo, sidebar, undo |
| `src/mapio.js` | parse and emit `.map` (pure — no DOM, no three.js) |
| `src/entity.js`, `src/brush.js`, `src/snap.js` | icons, meshes, grid snapping |
| `src/vocab.gen.js` | **generated** from `tools/schema/level_vocab.json` |
| `src/roundtrip.js` | the node entry point `mapmaker-roundtrip.nix` drives |
| `mapfmt.py` | the Python twin of `mapio.js`, plus `analyse()` |
| `mapgen.py` | `emit` / `dump` / `canon` / `example` / `classes` |
| `validate.py` | a printer over `mapfmt.analyse` |
| `tscn_map.py` | Redot `.tscn` <-> `.map`. Also where the axis, scale and yaw conventions are written down |
| `test_tscn_map.py`, `fixtures/` | those conventions proven, and the wrong ones refused |
| `../redot/addons/kiln_map` | the editor dock: the engine's budget, live, as you build |

`mapio.js` and `mapfmt.py` are held byte-identical by `mapmaker-roundtrip.nix`.
Both take their face table and their limits from `tools/schema/level_vocab.json`,
so there is one statement of the format and `nix/checks/level-vocab.nix` fails
if any copy drifts.

## Gates

`nix build .#checks.$SYS.{mapmaker-roundtrip,level-vocab,kiln-map,kiln-maprender,tscn-map}`,
or just `./dev cheap`, which runs them all in a few seconds.

`tscn-map` is also the bit-for-bit gate: it rebuilds `fixtures/redot_level.map`
from its `.tscn` and diffs it byte for byte, so the same scene must produce the
same level on every machine. That is why `tscn_map.py` keeps libm's
transcendentals off the output path — `atan2` and `acos` are not correctly
rounded and differ between libm implementations, where `+ - * /` and `sqrt`
are pinned by IEEE-754.
