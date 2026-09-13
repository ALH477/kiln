// SPDX-License-Identifier: MIT
// mapio.js — parse + emit Quake .map text in the canonical form kiln_map.c
// consumes. Pure, no three.js — the headless round-trip check imports this.
//
// Engine contract (engine/src/kiln/kiln_map.c):
//   - Entity = { epair* | brush* }
//   - Epair  = "key" "value"  (key ≤63, value ≤255 chars)
//   - Brush = { face* } ; Face = ( x y z ) ( x y z ) ( x y z ) tex ox oy rot sx sy
//   - AABB  = componentwise min/max of all 9 plane points across all 6 faces
//   - Trailing 6 tokens per face are required by hard count but ignored
//   - Limits: 256 brushes, 1536 faces, 64 spawns, 32 classnames; coords truncated
//     to int16. Round to integers before emitting so truncation is a no-op.
//
// Corner ordering for the 6 AABB faces MUST match assets/oot_test.map exactly —
// see VENDORED_WINDING note in vendor/README.md. The engine's AABB reduction is
// winding-independent, but tools/blender/quake_map.py's brush_to_faces CSG
// (used by validate.py) is winding-sensitive; wrong winding = inside-out CSG.

export { LIMITS } from './vocab.gen.js';
import { AABB_FACE_CORNERS } from './vocab.gen.js';

// Canonical 6-face-per-AABB corner ordering. Winding verified against
// quake_test.map:4-9 (the file mkQuakeMapModel round-trips through Blender, so
// quake_map.py's brush_to_faces CSG accepts it). The engine itself is
// winding-independent (it componentwise min/max's plane points), so any
// ordering parses on console, but tools/mapmaker/validate.py reuses
// quake_map.py's CSG and that needs outward normals — cross(p3-p1, p2-p1) must
// match the face's outward direction. oot_test.map is INSIDE-OUT relative to
// this convention (works on console, fails CSG); the editor's emit must use
// the canonical winding so its output validates.
function aabbFaces(mins, maxs) {
  // From the generated corner table, not a transcription. This was one of four
  // hand-kept copies of a winding whose sign decides whether a brush survives
  // the CSG at all — the others being validate.py, frg.py and forge_io.c.
  const sel = [mins, maxs];
  return AABB_FACE_CORNERS.map(
    f => f.map(c => [sel[c[0]][0], sel[c[1]][1], sel[c[2]][2]]));
}

function fmt(v) {
  return `${Math.round(v)}`;
}

function faceLine(p, tex) {
  return `( ${fmt(p[0][0])} ${fmt(p[0][1])} ${fmt(p[0][2])} ) `
       + `( ${fmt(p[1][0])} ${fmt(p[1][1])} ${fmt(p[1][2])} ) `
       + `( ${fmt(p[2][0])} ${fmt(p[2][1])} ${fmt(p[2][2])} ) `
       + `${tex} 0 0 0 1 1`;
}

export function emitMap(state) {
  const lines = [];
  lines.push('{');
  lines.push('"classname" "worldspawn"');
  for (const [k, v] of Object.entries(state.worldspawn || {})) {
    if (k !== 'classname') lines.push(`"${k}" "${v}"`);
  }
  for (const b of state.brushes) {
    const mins = b.mins, maxs = b.maxs;
    const tex = b.texture || 'TEX';
    lines.push('{');
    for (const f of aabbFaces(mins, maxs)) lines.push(faceLine(f, tex));
    lines.push('}');
  }
  lines.push('}');
  for (const s of state.spawns) {
    lines.push('{');
    lines.push(`"classname" "${s.classname}"`);
    if (s.origin) {
      const [x, y, z] = s.origin;
      lines.push(`"origin" "${fmt(x)} ${fmt(y)} ${fmt(z)}"`);
    }
    if (s.angle !== null && s.angle !== undefined) {
      lines.push(`"angle" "${Math.round(s.angle)}"`);
    }
    for (const e of s.epairs || []) {
      // Skip the canonical ones we already emitted.
      if (e.k === 'classname' || e.k === 'origin' || e.k === 'angle') continue;
      lines.push(`"${e.k}" "${e.v}"`);
    }
    lines.push('}');
  }
  return lines.join('\n') + '\n';
}

// ── parser ──────────────────────────────────────────────────────────────────

class Tok {
  constructor(text) {
    this.t = text;
    this.i = 0;
    this.n = text.length;
  }
  skipWS() {
    while (this.i < this.n) {
      const c = this.t[this.i];
      if (c === ' ' || c === '\t' || c === '\r' || c === '\n') this.i++;
      else if (c === '/' && this.t[this.i + 1] === '/') {
        while (this.i < this.n && this.t[this.i] !== '\n') this.i++;
      } else break;
    }
  }
  peek() { this.skipWS(); return this.i < this.n ? this.t[this.i] : ''; }
  next() {
    this.skipWS();
    if (this.i >= this.n) return null;
    const c = this.t[this.i];
    if (c === '{' || c === '}' || c === '(' || c === ')') { this.i++; return c; }
    if (c === '"') {
      const end = this.t.indexOf('"', this.i + 1);
      if (end < 0) throw new Error(`unterminated string at ${this.i}`);
      const s = this.t.slice(this.i + 1, end);
      this.i = end + 1;
      return { str: s };
    }
    // bareword: read until whitespace/brace/paren
    const start = this.i;
    while (this.i < this.n) {
      const cc = this.t[this.i];
      if (cc === ' ' || cc === '\t' || cc === '\r' || cc === '\n'
          || cc === '{' || cc === '}' || cc === '(' || cc === ')' || cc === '"') break;
      this.i++;
    }
    return this.t.slice(start, this.i);
  }
  expect(c) {
    const t = this.next();
    if (t !== c) throw new Error(`expected '${c}' at ${this.i}, got ${JSON.stringify(t)}`);
  }
  num() {
    const t = this.next();
    if (typeof t !== 'string') throw new Error(`expected number at ${this.i}`);
    return parseFloat(t);
  }
}

function parseBrushFace(tok) {
  tok.expect('(');
  const x1 = tok.num(), y1 = tok.num(), z1 = tok.num();
  tok.expect(')');
  tok.expect('(');
  const x2 = tok.num(), y2 = tok.num(), z2 = tok.num();
  tok.expect(')');
  tok.expect('(');
  const x3 = tok.num(), y3 = tok.num(), z3 = tok.num();
  tok.expect(')');
  const tex = tok.next();
  // 5 more trailing tokens: ox oy rot sx sy (required by hard count)
  for (let k = 0; k < 5; k++) tok.next();
  return {
    p1: [x1, y1, z1], p2: [x2, y2, z2], p3: [x3, y3, z3],
    texture: typeof tex === 'string' ? tex : 'TEX',
  };
}

function aabbFromFaces(faces) {
  let mn = [Infinity, Infinity, Infinity];
  let mx = [-Infinity, -Infinity, -Infinity];
  for (const f of faces) {
    for (const p of [f.p1, f.p2, f.p3]) {
      for (let i = 0; i < 3; i++) {
        if (p[i] < mn[i]) mn[i] = p[i];
        if (p[i] > mx[i]) mx[i] = p[i];
      }
    }
  }
  return { mins: mn, maxs: mx };
}

export function parseMap(text) {
  const tok = new Tok(text);
  const entities = [];
  tok.skipWS();
  while (tok.i < tok.n) {
    tok.expect('{');
    const ent = { props: {}, brushes: [] };
    let done = false;
    while (!done) {
      const c = tok.peek();
      if (c === '}') { tok.next(); done = true; }
      else if (c === '{') {
        tok.next();
        const faces = [];
        while (tok.peek() !== '}') {
          if (tok.peek() === '(') faces.push(parseBrushFace(tok));
          else throw new Error(`unexpected token in brush at ${tok.i}`);
        }
        tok.expect('}');
        ent.brushes.push(faces);
      } else if (c === '"') {
        const k = tok.next().str;
        const v = tok.next().str;
        ent.props[k] = v;
      } else {
        throw new Error(`unexpected token '${c}' in entity at ${tok.i}`);
      }
    }
    entities.push(ent);
    tok.skipWS();
  }
  return entities;
}

// Flatten the parsed entities into the editor's state shape:
//   { brushes: [{mins,maxs,texture}], spawns: [{classname,origin,angle,epairs}] }
// worldspawn is treated as the brush owner; every entity with brushes
// contributes its brushes flat; every entity without brushes is a spawn.
export function toEditorState(entities) {
  const brushes = [];
  const spawns = [];
  const worldspawn = {};
  for (const ent of entities) {
    if (ent.brushes.length > 0) {
      // Worldspawn's own epairs used to be dropped here — this branch only
      // ever looked at the brushes. Kept in step with mapfmt.py's to_state.
      for (const [k, v] of Object.entries(ent.props)) {
        if (k !== 'classname' && !(k in worldspawn)) worldspawn[k] = v;
      }
      for (const faces of ent.brushes) {
        const aabb = aabbFromFaces(faces);
        aabb.texture = faces[0]?.texture || 'TEX';
        brushes.push(aabb);
      }
    } else {
      const props = ent.props;
      const origin = (props.origin || '0 0 0').split(/\s+/).map(parseFloat);
      while (origin.length < 3) origin.push(0);
      const angle = props.angle !== undefined ? parseInt(props.angle, 10) : 0;
      const epairs = [];
      for (const [k, v] of Object.entries(props)) {
        if (k === 'classname' || k === 'origin' || k === 'angle') continue;
        epairs.push({ k, v });
      }
      spawns.push({
        classname: props.classname || 'info_player_start',
        origin, angle, epairs,
      });
    }
  }
  const st = { brushes, spawns };
  if (Object.keys(worldspawn).length > 0) st.worldspawn = worldspawn;
  return st;
}

export function parseEditorState(text) {
  return toEditorState(parseMap(text));
}