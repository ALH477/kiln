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
import { AABB_FACE_CORNERS, LIMITS } from './vocab.gen.js';

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

// ── convex brushes ──────────────────────────────────────────────────────────
// The Python twin of all of this is tools/mapmaker/mapfmt.py's convex section,
// and mapmaker-roundtrip.nix holds the two in agreement. Read that file for
// WHY the authored form is a point set and never a plane list; the short
// version is that an author who never states a winding cannot state one
// backwards, and six of this repo's seven committed .map files were wound
// inward before anything noticed.
//
// The browser editor's own gizmos are box-only, so it will not let you drag a
// convex brush into a new shape. What matters here is that opening a level
// that HAS one and saving it again does not quietly turn a wedge into a block.

const HULL_EPS = 1e-3;
const EPS = 1e-6;

const vsub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const vdot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const vcross = (a, b) => [a[1] * b[2] - a[2] * b[1],
                          a[2] * b[0] - a[0] * b[2],
                          a[0] * b[1] - a[1] * b[0]];
const vlen = a => Math.sqrt(vdot(a, a));
function vnorm(a) {
  const l = vlen(a);
  return l < EPS ? null : [a[0] / l, a[1] / l, a[2] / l];
}

// cross(p3-p1, p2-p1) — the outward convention, same as quake_map.py's
// plane_normal_dist and kiln_map.c's plane_from_points.
function planeNormalDist(f) {
  const n = vnorm(vcross(vsub(f.p3, f.p1), vsub(f.p2, f.p1)));
  if (!n) return null;
  return { n, d: vdot(n, f.p1) };
}

function intersect3(a, b, c) {
  const det = vdot(a.n, vcross(b.n, c.n));
  if (Math.abs(det) < EPS) return null;
  const c23 = vcross(b.n, c.n), c31 = vcross(c.n, a.n), c12 = vcross(a.n, b.n);
  return [0, 1, 2].map(i =>
    (a.d * c23[i] + b.d * c31[i] + c.d * c12[i]) / det);
}

const ptKey = p => p.map(c => Math.round(c * 1e4) / 1e4).join(',');

function dedupePoints(pts) {
  const seen = new Set(), out = [];
  for (const p of pts) {
    const k = ptKey(p);
    if (!seen.has(k)) { seen.add(k); out.push(p); }
  }
  // Sorted, so the hull is a SET and the order it was discovered in never
  // reaches the file. mapfmt.py sorts for the same reason.
  out.sort((a, b) => a[0] - b[0] || a[1] - b[1] || a[2] - b[2]);
  return out;
}

// The vertices a set of planes bounds: every triple that meets at a point
// inside every other plane. kiln_map.c's CSG run in this direction too.
function brushVertices(faces) {
  const planes = [];
  for (const f of faces) {
    const pl = planeNormalDist(f);
    if (pl) planes.push(pl);
  }
  const out = [];
  for (let i = 0; i < planes.length; i++)
    for (let j = i + 1; j < planes.length; j++)
      for (let k = j + 1; k < planes.length; k++) {
        const v = intersect3(planes[i], planes[j], planes[k]);
        if (!v) continue;
        let inside = true;
        for (const q of planes)
          if (vdot(q.n, v) > q.d + HULL_EPS) { inside = false; break; }
        if (inside) out.push(v);
      }
  return dedupePoints(out);
}

// The box six axial planes bound, ignoring winding, or null. The twin of
// mapfmt.py's box_from_planes, and the test that decides box vs convex.
function boxFromPlanes(faces) {
  const lo = [null, null, null], hi = [null, null, null];
  for (const f of faces) {
    const pl = planeNormalDist(f);
    if (!pl) return null;
    let ax = 0;
    for (let i = 1; i < 3; i++)
      if (Math.abs(pl.n[i]) > Math.abs(pl.n[ax])) ax = i;
    if (Math.abs(pl.n[ax]) < EPS) return null;
    let zeros = 0;
    for (const c of pl.n) if (Math.abs(c) < EPS) zeros++;
    if (zeros !== 2) return null;
    const val = pl.d / pl.n[ax];
    if (lo[ax] === null) lo[ax] = val;
    else if (hi[ax] === null) hi[ax] = val;
    else return null;
  }
  for (let i = 0; i < 3; i++) if (lo[i] === null || hi[i] === null) return null;
  const mins = [0, 1, 2].map(i => Math.min(lo[i], hi[i]));
  const maxs = [0, 1, 2].map(i => Math.max(lo[i], hi[i]));
  for (let i = 0; i < 3; i++) if (maxs[i] - mins[i] < EPS) return null;
  return { mins, maxs };
}

function spanningTriple(onPlane, n) {
  if (onPlane.length < 3) return null;
  let best = null, bestArea = 0;
  for (let ai = 0; ai < onPlane.length; ai++)
    for (let bi = ai + 1; bi < onPlane.length; bi++)
      for (let ci = bi + 1; ci < onPlane.length; ci++) {
        let a = onPlane[ai], b = onPlane[bi], c = onPlane[ci];
        const m = vcross(vsub(b, a), vsub(c, a));
        const area = vlen(m);
        if (area <= bestArea) continue;
        if (vdot(m, n) < 0) { const t = b; b = c; c = t; }
        best = [a, b, c]; bestArea = area;
      }
  return best;
}

function hullOnce(points, tex) {
  const pts = dedupePoints(points.map(p => [+p[0], +p[1], +p[2]]));
  if (pts.length < 4)
    throw new Error(`mapio: a convex brush needs 4+ distinct points, got ${pts.length}`);
  const cen = [0, 1, 2].map(i => pts.reduce((s, p) => s + p[i], 0) / pts.length);
  const found = new Map();
  for (let i = 0; i < pts.length; i++)
    for (let j = i + 1; j < pts.length; j++)
      for (let k = j + 1; k < pts.length; k++) {
        let n = vnorm(vcross(vsub(pts[j], pts[i]), vsub(pts[k], pts[i])));
        if (!n) continue;
        let d = vdot(n, pts[i]);
        if (vdot(n, cen) > d) { n = n.map(c => -c); d = -d; }
        let bounds = true;
        for (const p of pts) if (vdot(n, p) > d + HULL_EPS) { bounds = false; break; }
        if (!bounds) continue;
        const key = n.map(c => Math.round(c * 1e3) / 1e3).join(',')
                  + ',' + (Math.round(d * 1e3) / 1e3);
        if (!found.has(key)) found.set(key, { n, d });
      }
  if (found.size < 4)
    throw new Error(`mapio: a convex brush's ${pts.length} points bound `
      + `${found.size} planes; 4+ are needed for a closed volume`);
  if (found.size > LIMITS.brush_planes)
    throw new Error(`mapio: a convex brush needs ${found.size} planes; `
      + `kiln_map.c holds ${LIMITS.brush_planes} and drops the rest`);

  const ordered = [...found.values()].sort(
    (a, b) => a.n[0] - b.n[0] || a.n[1] - b.n[1] || a.n[2] - b.n[2] || a.d - b.d);
  const out = [];
  for (const { n, d } of ordered) {
    const on = pts.filter(p => Math.abs(vdot(n, p) - d) < HULL_EPS);
    const tri = spanningTriple(on, n);
    if (!tri) continue;
    const [a, b, c] = tri.map(q => q.map(v => Math.round(v)));
    // plane_normal_dist reads cross(p3-p1, p2-p1): a CCW-from-outside triple
    // is emitted with p2 and p3 swapped.
    out.push({ p1: a, p2: c, p3: b, texture: tex });
  }
  return out;
}

// Iterated to its fixed point, for the reason mapfmt.py's hull_planes spells
// out: the plane reaches the file as three INTEGER points and is re-derived
// from them, so one pass is not stable and emit|parse|emit produced a second,
// different file.
export function hullPlanes(points, tex) {
  let planes = hullOnce(points, tex);
  for (let i = 0; i < 4; i++) {
    const next = hullOnce(brushVertices(planes), tex);
    const same = next.length === planes.length && next.every((q, j) =>
      JSON.stringify([q.p1, q.p2, q.p3]) ===
      JSON.stringify([planes[j].p1, planes[j].p2, planes[j].p3]));
    if (same) return planes;
    planes = next;
  }
  throw new Error('mapio: a convex brush\'s hull did not settle to integer '
    + 'coordinates in 4 passes');
}

export function emitMap(state) {
  const lines = [];
  lines.push('{');
  lines.push('"classname" "worldspawn"');
  for (const [k, v] of Object.entries(state.worldspawn || {})) {
    if (k !== 'classname') lines.push(`"${k}" "${v}"`);
  }
  for (const b of state.brushes) {
    const tex = b.texture || 'TEX';
    lines.push('{');
    if (b.convex) {
      // The hull derives its own outward normals, so this branch can no more
      // emit an inward-wound brush than the box branch can.
      for (const pl of hullPlanes(b.convex, tex))
        lines.push(faceLine([pl.p1, pl.p2, pl.p3], tex));
    } else {
      for (const f of aabbFaces(b.mins, b.maxs)) lines.push(faceLine(f, tex));
    }
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
        const tex = faces[0]?.texture || 'TEX';
        // A brush the six-axial-planes reduction cannot describe is not a box,
        // and flattening it to its AABB here MOVED GEOMETRY -- opening a level
        // with a 45-degree wall and saving it turned that wall into a block,
        // silently. Carry the real hull instead, and fall back to the box only
        // when the brush yields no solid at all, which is the case `mapgen
        // canon` exists to repair.
        if (boxFromPlanes(faces) === null) {
          const pts = brushVertices(faces);
          if (pts.length >= 4) {
            brushes.push({ convex: pts.map(p => p.map(c => Math.round(c))), texture: tex });
            continue;
          }
        }
        const aabb = aabbFromFaces(faces);
        aabb.texture = tex;
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