// SPDX-License-Identifier: MIT
// main.js — three.js map maker orchestrator. Scene + camera + controls, mode
// switching (select / create-brush / place-spawn), mouse picking, sidebar UI
// binding, import/export, client-side validation, undo/redo.

import * as THREE from 'three';
import { TransformControls } from 'three/addons/controls/TransformControls.js';
import { CSS2DRenderer } from 'three/addons/renderers/CSS2DRenderer.js';
import { createViewport } from 'webcommon/viewport.js';
import { StudioFile, describeSaveError, download, saveOrAsk, studioParams } from 'webcommon/io.js';

import { allocId as allocBrushId, makeBrushMesh, syncBrushMesh } from './brush.js';
import { allocId as allocSpawnId, makeSpawnGroup, syncSpawnGroup, KNOWN_CLASSNAMES, ENTITY_PALETTE } from './entity.js';
import { parseEditorState, emitMap, LIMITS } from './mapio.js';
import { snapRound, snapFloor, snapCeil, clampInt16 } from './snap.js';
// EPAIR_SCHEMAS used to be a local table in this file; df31be7 moved it into
// the generated vocabulary and deleted the table but not the use (line ~562),
// so selecting any spawn threw a ReferenceError. No gate loads main.js.
import { EPAIR_SCHEMAS } from './vocab.gen.js';

const DEFAULT_GRID = 16;
const DEFAULT_BRUSH_HEIGHT = 64;

const state = {
  brushes: [],
  spawns: [],
  grid: DEFAULT_GRID,
  mode: 'select',
  selection: null, // { kind, id }
};
const meshes = { brushes: new Map(), spawns: new Map() };
const undo = [];
const redo = [];

// ── three.js scene ────────────────────────────────────────────────────────────

const viewport = document.getElementById('viewport');
const W = () => viewport.clientWidth;
const H = () => viewport.clientHeight;

const view = createViewport(viewport, {
  background: 0x1c2228, fov: 50, near: 1, far: 20000,
  eye: [200, 200, 200], target: [0, 16, 0], damping: 0.1,
});
const { renderer, scene, camera, orbit } = view;

const labelRenderer = new CSS2DRenderer();
labelRenderer.domElement.style.position = 'absolute';
labelRenderer.domElement.style.top = '0';
labelRenderer.domElement.style.pointerEvents = 'none';
viewport.appendChild(labelRenderer.domElement);
view.overlay(labelRenderer);

const gridHelper = new THREE.GridHelper(2048, 128, 0x445566, 0x2a3338);
scene.add(gridHelper);

// Invisible ground plane for raycasting click-drag onto y=0.
const groundPlane = new THREE.Plane(new THREE.Vector3(0, 1, 0), 0);

const amb = new THREE.AmbientLight(0xffffff, 0.6);
scene.add(amb);
const dir = new THREE.DirectionalLight(0xffffff, 0.8);
dir.position.set(100, 200, 80);
scene.add(dir);

const transform = new TransformControls(camera, renderer.domElement);
transform.addEventListener('dragging-changed', e => {
  orbit.enabled = !e.value;
  if (e.value) {
    // Drag STARTED — snapshot before anything moves, or the gizmo is the one
    // edit in the editor that ctrl-Z cannot reach. Every other mutation goes
    // through pushUndo(); this path did not.
    pushUndo();
  } else {
    // Drag ended — bake mesh position back to brush AABB.
    applyTransformToBrush();
  }
});
scene.add(transform);

// Create-mode preview box.
const previewMat = new THREE.MeshStandardMaterial({
  color: 0x33ddaa, transparent: true, opacity: 0.35,
  depthWrite: false, wireframe: false,
});
const previewBox = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1), previewMat);
previewBox.visible = false;
scene.add(previewBox);

const raycaster = new THREE.Raycaster();
const mouseN = new THREE.Vector2();

// ── state mutations ───────────────────────────────────────────────────────────

function snapshot() {
  return JSON.parse(JSON.stringify({
    brushes: state.brushes,
    spawns: state.spawns,
  }));
}
function pushUndo() {
  undo.push(snapshot());
  if (undo.length > 40) undo.shift();
  redo.length = 0;
}
function doUndo() {
  if (undo.length === 0) return;
  redo.push(snapshot());
  const s = undo.pop();
  state.brushes = s.brushes;
  state.spawns = s.spawns;
  state.selection = null;
  rebuildScene();
  syncSidebar();
}
function doRedo() {
  if (redo.length === 0) return;
  undo.push(snapshot());
  const s = redo.pop();
  state.brushes = s.brushes;
  state.spawns = s.spawns;
  state.selection = null;
  rebuildScene();
  syncSidebar();
}

function selectedBrush() {
  if (!state.selection || state.selection.kind !== 'brush') return null;
  return state.brushes.find(b => b.id === state.selection.id);
}
function selectedSpawn() {
  if (!state.selection || state.selection.kind !== 'spawn') return null;
  return state.spawns.find(s => s.id === state.selection.id);
}

// ── scene rebuild ─────────────────────────────────────────────────────────────

function rebuildScene() {
  // brushes
  const liveBrushIds = new Set(state.brushes.map(b => b.id));
  for (const [id, mesh] of meshes.brushes) {
    if (!liveBrushIds.has(id)) {
      scene.remove(mesh);
      mesh.geometry.dispose();
      mesh.material.dispose();
      meshes.brushes.delete(id);
    }
  }
  for (const b of state.brushes) {
    const sel = state.selection && state.selection.kind === 'brush' && state.selection.id === b.id;
    let mesh = meshes.brushes.get(b.id);
    if (!mesh) {
      mesh = makeBrushMesh(b, sel);
      meshes.brushes.set(b.id, mesh);
      scene.add(mesh);
    }
    syncBrushMesh(mesh, b, sel);
  }
  // spawns
  const liveSpawnIds = new Set(state.spawns.map(s => s.id));
  for (const [id, group] of meshes.spawns) {
    if (!liveSpawnIds.has(id)) {
      scene.remove(group);
      meshes.spawns.delete(id);
    }
  }
  for (const s of state.spawns) {
    const sel = state.selection && state.selection.kind === 'spawn' && state.selection.id === s.id;
    let group = meshes.spawns.get(s.id);
    if (!group) {
      group = makeSpawnGroup(s, sel);
      meshes.spawns.set(s.id, group);
      scene.add(group);
    }
    syncSpawnGroup(group, s, sel);
  }
  // TransformControls attachment
  if (state.selection && state.selection.kind === 'brush') {
    const m = meshes.brushes.get(state.selection.id);
    if (m && !transform.dragging) transform.attach(m);
  } else {
    if (!transform.dragging) transform.detach();
  }
}

function applyTransformToBrush() {
  const b = selectedBrush();
  if (!b) return;
  const mesh = meshes.brushes.get(b.id);
  if (!mesh) return;
  const sx = b.maxs[0] - b.mins[0];
  const sy = b.maxs[1] - b.mins[1];
  const sz = b.maxs[2] - b.mins[2];
  let cx = snapRound(mesh.position.x, state.grid);
  let cy = snapRound(mesh.position.y, state.grid);
  let cz = snapRound(mesh.position.z, state.grid);
  b.mins = [clampInt16(cx - sx / 2), clampInt16(cy - sy / 2), clampInt16(cz - sz / 2)];
  b.maxs = [clampInt16(cx + sx / 2), clampInt16(cy + sy / 2), clampInt16(cz + sz / 2)];
  syncBrushMesh(mesh, b, true);
  syncSidebar();
}

// ── mouse picking ────────────────────────────────────────────────────────────

function updateMouse(ev) {
  const rect = renderer.domElement.getBoundingClientRect();
  mouseN.x = ((ev.clientX - rect.left) / rect.width) * 2 - 1;
  mouseN.y = -((ev.clientY - rect.top) / rect.height) * 2 + 1;
}

function pickObject() {
  raycaster.setFromCamera(mouseN, camera);
  const candidates = [...meshes.brushes.values(), ...meshes.spawns.values()];
  const hits = raycaster.intersectObjects(candidates, true);
  for (const h of hits) {
    let o = h.object;
    while (o && !o.userData.kind) o = o.parent;
    if (o) return o;
  }
  return null;
}

function pickGround() {
  raycaster.setFromCamera(mouseN, camera);
  const v = new THREE.Vector3();
  raycaster.ray.intersectPlane(groundPlane, v);
  return v;
}

// ── mode: create-brush ────────────────────────────────────────────────────────

let creating = null; // { startX, startZ }

function beginCreate(ev) {
  updateMouse(ev);
  const p = pickGround();
  if (!p) return;
  creating = {
    startX: snapFloor(p.x, state.grid),
    startZ: snapFloor(p.z, state.grid),
  };
  previewBox.visible = true;
}
function moveCreate(ev) {
  if (!creating) return;
  updateMouse(ev);
  const p = pickGround();
  if (!p) return;
  const x0 = creating.startX;
  const z0 = creating.startZ;
  const x1 = snapCeil(p.x, state.grid);
  const z1 = snapCeil(p.z, state.grid);
  const mn = [Math.min(x0, x1), 0, Math.min(z0, z1)];
  const mx = [Math.max(x0, x1), DEFAULT_BRUSH_HEIGHT, Math.max(z0, z1)];
  previewBox.position.set((mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2);
  previewBox.scale.set(
    Math.max(0.001, mx[0] - mn[0]),
    Math.max(0.001, mx[1] - mn[1]),
    Math.max(0.001, mx[2] - mn[2]),
  );
}
function endCreate() {
  if (!creating) return;
  const x0 = creating.startX;
  const z0 = creating.startZ;
  previewBox.visible = false;
  // Use the current preview box scale/position to derive the AABB.
  const sx = Math.abs(previewBox.scale.x);
  const sz = Math.abs(previewBox.scale.z);
  if (sx < 1 || sz < 1) { creating = null; return; }
  const cx = snapRound(previewBox.position.x, state.grid);
  const cz = snapRound(previewBox.position.z, state.grid);
  const mins = [cx - sx / 2, 0, cz - sz / 2];
  const maxs = [cx + sx / 2, DEFAULT_BRUSH_HEIGHT, cz + sz / 2];
  pushUndo();
  const brush = { id: allocBrushId(), mins, maxs, texture: 'TEX' };
  state.brushes.push(brush);
  state.selection = { kind: 'brush', id: brush.id };
  creating = null;
  rebuildScene();
  syncSidebar();
}

// ── mode: place-spawn ─────────────────────────────────────────────────────────

function placeSpawnAtGround(ev) {
  updateMouse(ev);
  const p = pickGround();
  if (!p) return;
  pushUndo();
  const x = snapRound(p.x, state.grid);
  const z = snapRound(p.z, state.grid);
  const spawn = {
    id: allocSpawnId(),
    classname: 'info_player_start',
    origin: [x, 16, z],
    angle: 0,
    epairs: [],
  };
  state.spawns.push(spawn);
  state.selection = { kind: 'spawn', id: spawn.id };
  rebuildScene();
  syncSidebar();
}

// ── mouse dispatch ────────────────────────────────────────────────────────────

renderer.domElement.addEventListener('mousedown', ev => {
  if (ev.button !== 0) return;
  if (state.mode === 'create-brush') beginCreate(ev);
  else if (state.mode === 'place-spawn') placeSpawnAtGround(ev);
});
renderer.domElement.addEventListener('mousemove', ev => {
  if (state.mode === 'create-brush') moveCreate(ev);
});
renderer.domElement.addEventListener('mouseup', ev => {
  if (ev.button !== 0) return;
  if (state.mode === 'create-brush') endCreate();
});
renderer.domElement.addEventListener('click', ev => {
  if (state.mode !== 'select') return;
  if (transform.dragging) return;
  updateMouse(ev);
  const o = pickObject();
  if (o) state.selection = { kind: o.userData.kind, id: o.userData.id };
  else state.selection = null;
  rebuildScene();
  syncSidebar();
});

// ── sidebar UI ────────────────────────────────────────────────────────────────

const $ = id => document.getElementById(id);
const selBox = $('selection');
const modeBtns = document.querySelectorAll('.mode-btn');
const gridBtns = document.querySelectorAll('.grid-btn');

modeBtns.forEach(btn => btn.addEventListener('click', () => {
  state.mode = btn.dataset.mode;
  modeBtns.forEach(b => b.classList.toggle('active', b === btn));
  previewBox.visible = false;
  creating = null;
  if (state.mode !== 'select') {
    state.selection = null;
    rebuildScene();
    syncSidebar();
  }
  renderer.domElement.style.cursor = state.mode === 'select' ? 'default' : 'crosshair';
}));

gridBtns.forEach(btn => btn.addEventListener('click', () => {
  state.grid = parseInt(btn.dataset.grid, 10);
  gridBtns.forEach(b => b.classList.toggle('active', b === btn));
}));

$('undo').addEventListener('click', doUndo);
$('redo').addEventListener('click', doRedo);

$('import').addEventListener('click', () => $('import-file').click());
function loadMapText(text) {
  const s = parseEditorState(text);
  // Map editor ids onto imported items.
  s.brushes.forEach(b => b.id = allocBrushId());
  s.spawns.forEach(sp => sp.id = allocSpawnId());
  pushUndo();
  state.brushes = s.brushes;
  state.spawns = s.spawns;
  state.selection = null;
  rebuildScene();
  syncSidebar();
}

$('import-file').addEventListener('change', ev => {
  const f = ev.target.files[0];
  if (!f) return;
  f.text().then(text => {
    try {
      loadMapText(text);
    } catch (e) {
      alert('import failed: ' + e.message);
    }
  });
  ev.target.value = '';
});

// Standalone, export is a download. Opened from Kiln Studio (?studio=1&file=),
// the same button saves the file in place — refused if someone else saved it
// since it was opened — and the studio runs map-validate on the result.
const studio = studioParams();
const studioFile = studio && studio.file ? new StudioFile(studio.file) : null;
const fileOut = (msg, cls = '') => { $('file-out').textContent = msg || ''; $('file-out').className = `hint ${cls}`; };

$('export').addEventListener('click', async () => {
  const text = emitMap(state);
  if (!studioFile) return download('level.map', text);
  try {
    const r = await saveOrAsk(studioFile, text);
    fileOut(`saved ${studioFile.path}` + (r.job ? ' — map-validate is running (Jobs)' : ''), 'ok');
  } catch (e) {
    fileOut(describeSaveError(e), 'bad');
  }
});

if (studioFile) {
  $('export').textContent = 'save';
  studioFile.open().then(d => {
    if (d.text != null) loadMapText(d.text);
    fileOut(d.text == null ? `${studioFile.path} is new — save creates it` : `editing ${studioFile.path}`);
    studioFile.hold(err => { if (err) fileOut(err.message + ' — saving will ask before taking it over', 'bad'); });
  }, e => fileOut(`could not open ${studioFile.path}: ${e.message}`, 'bad'));
}

$('validate').addEventListener('click', () => {
  const report = clientValidate();
  $('validate-out').textContent = report;
});

function clientValidate() {
  const lines = [];
  lines.push(`brushes: ${state.brushes.length} / ${LIMITS.brushes}`);
  lines.push(`spawns:  ${state.spawns.length} / ${LIMITS.spawns}`);
  let bad = 0;
  for (const b of state.brushes) {
    for (let i = 0; i < 3; i++) {
      if (b.mins[i] > b.maxs[i]) { bad++; break; }
      if (Math.abs(b.mins[i]) > LIMITS.coord || Math.abs(b.maxs[i]) > LIMITS.coord) { bad++; break; }
    }
  }
  if (bad) lines.push(`WARN: ${bad} brush(es) degenerate or out of ±${LIMITS.coord}`);
  const classnames = new Set(state.spawns.map(s => s.classname));
  lines.push(`classnames: ${classnames.size} / ${LIMITS.classnames}`);
  return lines.join('\n');
}

function syncSidebar() {
  selBox.innerHTML = '';
  const b = selectedBrush();
  const s = selectedSpawn();
  if (b) sidebarBrush(b);
  else if (s) sidebarSpawn(s);
  else {
    const empty = document.createElement('p');
    empty.textContent = 'nothing selected';
    empty.className = 'muted';
    selBox.appendChild(empty);
  }
}

function num(label, value, onInput) {
  const lab = document.createElement('label');
  lab.textContent = label;
  const inp = document.createElement('input');
  inp.type = 'number';
  inp.value = Math.round(value);
  inp.size = 4;
  inp.addEventListener('change', () => {
    const v = clampInt16(parseInt(inp.value, 10));
    onInput(v);
    rebuildScene();
    syncSidebar();
  });
  const wrap = document.createElement('div');
  wrap.className = 'row';
  wrap.appendChild(lab);
  wrap.appendChild(inp);
  return wrap;
}

function sidebarBrush(b) {
  const title = document.createElement('h3');
  title.textContent = `brush #${b.id}`;
  selBox.appendChild(title);
  const minsWrap = document.createElement('div');
  minsWrap.appendChild(num('min.x', b.mins[0], v => { pushUndo(); b.mins[0] = v; if (b.mins[0] > b.maxs[0]) b.maxs[0] = b.mins[0]; }));
  minsWrap.appendChild(num('min.y', b.mins[1], v => { pushUndo(); b.mins[1] = v; if (b.mins[1] > b.maxs[1]) b.maxs[1] = b.mins[1]; }));
  minsWrap.appendChild(num('min.z', b.mins[2], v => { pushUndo(); b.mins[2] = v; if (b.mins[2] > b.maxs[2]) b.maxs[2] = b.mins[2]; }));
  selBox.appendChild(minsWrap);
  const maxsWrap = document.createElement('div');
  maxsWrap.appendChild(num('max.x', b.maxs[0], v => { pushUndo(); b.maxs[0] = v; if (b.maxs[0] < b.mins[0]) b.mins[0] = b.maxs[0]; }));
  maxsWrap.appendChild(num('max.y', b.maxs[1], v => { pushUndo(); b.maxs[1] = v; if (b.maxs[1] < b.mins[1]) b.mins[1] = b.maxs[1]; }));
  maxsWrap.appendChild(num('max.z', b.maxs[2], v => { pushUndo(); b.maxs[2] = v; if (b.maxs[2] < b.mins[2]) b.mins[2] = b.maxs[2]; }));
  selBox.appendChild(maxsWrap);
  const tex = document.createElement('input');
  tex.value = b.texture || 'TEX';
  tex.placeholder = 'texture name (free token)';
  tex.addEventListener('change', () => { pushUndo(); b.texture = tex.value || 'TEX'; });
  const texWrap = document.createElement('div');
  texWrap.className = 'row';
  texWrap.appendChild(document.createElement('label')).textContent = 'texture';
  texWrap.appendChild(tex);
  selBox.appendChild(texWrap);
  const del = document.createElement('button');
  del.textContent = 'delete brush';
  del.addEventListener('click', () => {
    pushUndo();
    state.brushes = state.brushes.filter(x => x.id !== b.id);
    state.selection = null;
    rebuildScene();
    syncSidebar();
  });
  selBox.appendChild(del);
}

// ── entity-specific epair schemas ────────────────────────────────────────────
// For each classname that needs typed fields, define the schema.
// 'generic' epairs are still available below the typed fields.
// Generated from tools/schema/level_vocab.json. The TYPE RENDERING
// (text/number/select/vec3/door_select, below) stays here: data in the schema,
// behaviour in the editor.


function getEpair(spawn, key) {
  for (const e of (spawn.epairs || [])) if (e.k === key) return e.v;
  return null;
}
function setEpair(spawn, key, value) {
  if (!spawn.epairs) spawn.epairs = [];
  for (const e of spawn.epairs) {
    if (e.k === key) { e.v = value; return; }
  }
  spawn.epairs.push({ k: key, v: value });
}
function removeEpair(spawn, key) {
  if (!spawn.epairs) return;
  spawn.epairs = spawn.epairs.filter(e => e.k !== key);
}

function findDoorSpawns() {
  return state.spawns.filter(s => s.classname === 'info_key_door');
}

function sidebarSpawn(s) {
  const title = document.createElement('h3');
  title.textContent = `spawn #${s.id}`;
  selBox.appendChild(title);
  // classname
  const cnWrap = document.createElement('div');
  cnWrap.className = 'row';
  const cnLab = document.createElement('label');
  cnLab.textContent = 'classname';
  const cn = document.createElement('select');
  for (const k of KNOWN_CLASSNAMES) {
    if (k === 'worldspawn') continue;
    const opt = document.createElement('option');
    opt.value = k; opt.textContent = k;
    if (s.classname === k) opt.selected = true;
    cn.appendChild(opt);
  }
  const custom = document.createElement('option');
  custom.value = '__custom'; custom.textContent = '(custom)';
  cn.appendChild(custom);
  if (!KNOWN_CLASSNAMES.includes(s.classname)) cn.value = '__custom';
  cn.addEventListener('change', () => {
    pushUndo();
    if (cn.value === '__custom') {
      const v = prompt('classname:', s.classname);
      if (v) s.classname = v;
    } else {
      s.classname = cn.value;
    }
    rebuildScene();
    syncSidebar();
  });
  cnWrap.appendChild(cnLab); cnWrap.appendChild(cn);
  selBox.appendChild(cnWrap);
  // origin
  const oWrap = document.createElement('div');
  oWrap.appendChild(num('o.x', s.origin[0], v => { pushUndo(); s.origin[0] = v; }));
  oWrap.appendChild(num('o.y', s.origin[1], v => { pushUndo(); s.origin[1] = v; }));
  oWrap.appendChild(num('o.z', s.origin[2], v => { pushUndo(); s.origin[2] = v; }));
  selBox.appendChild(oWrap);
  // angle
  selBox.appendChild(num('angle°', s.angle || 0, v => { pushUndo(); s.angle = v; }));

  // ── entity-specific typed fields ──────────────────────────────────
  const schema = EPAIR_SCHEMAS[s.classname];
  if (schema) {
    const epTitle = document.createElement('h4');
    epTitle.textContent = `${s.classname} fields`;
    selBox.appendChild(epTitle);
    for (const field of schema) {
      if (field.type === 'text') {
        const wrap = document.createElement('div');
        wrap.className = 'row';
        const lab = document.createElement('label');
        lab.textContent = field.label;
        const inp = document.createElement('input');
        inp.type = 'text'; inp.value = getEpair(s, field.key) || field.default;
        inp.style.flex = '1 1 0';
        inp.addEventListener('change', () => {
          pushUndo(); setEpair(s, field.key, inp.value);
          rebuildScene(); syncSidebar();
        });
        wrap.appendChild(lab); wrap.appendChild(inp);
        selBox.appendChild(wrap);
      } else if (field.type === 'number') {
        const val = parseInt(getEpair(s, field.key) || field.default, 10);
        selBox.appendChild(num(field.label, val, v => {
          pushUndo(); setEpair(s, field.key, String(v));
        }));
      } else if (field.type === 'select') {
        const wrap = document.createElement('div');
        wrap.className = 'row';
        const lab = document.createElement('label');
        lab.textContent = field.label;
        const sel = document.createElement('select');
        for (const opt of field.options) {
          const o = document.createElement('option');
          o.value = opt.value; o.textContent = opt.label;
          if ((getEpair(s, field.key) || field.default) === opt.value) o.selected = true;
          sel.appendChild(o);
        }
        sel.addEventListener('change', () => {
          pushUndo(); setEpair(s, field.key, sel.value);
          rebuildScene(); syncSidebar();
        });
        wrap.appendChild(lab); wrap.appendChild(sel);
        selBox.appendChild(wrap);
      } else if (field.type === 'vec3') {
        const cur = getEpair(s, field.key) || field.default;
        const parts = cur.split(/\s+/).map(parseFloat);
        while (parts.length < 3) parts.push(0);
        const wrap = document.createElement('div');
        wrap.className = 'row';
        const lab = document.createElement('label');
        lab.textContent = field.label;
        wrap.appendChild(lab);
        for (let i = 0; i < 3; i++) {
          const inp = document.createElement('input');
          inp.type = 'number'; inp.value = Math.round(parts[i]); inp.style.width = '48px';
          inp.addEventListener('change', () => {
            parts[i] = parseInt(inp.value, 10) || 0;
            pushUndo(); setEpair(s, field.key, parts.join(' '));
            rebuildScene(); syncSidebar();
          });
          wrap.appendChild(inp);
        }
        selBox.appendChild(wrap);
      } else if (field.type === 'door_select') {
        const wrap = document.createElement('div');
        wrap.className = 'row';
        const lab = document.createElement('label');
        lab.textContent = field.label;
        const sel = document.createElement('select');
        const doors = findDoorSpawns();
        const noneOpt = document.createElement('option');
        noneOpt.value = '0'; noneOpt.textContent = '(none)';
        sel.appendChild(noneOpt);
        for (const d of doors) {
          const o = document.createElement('option');
          o.value = String(d.id); o.textContent = `door #${d.id}`;
          if ((getEpair(s, field.key) || field.default) === String(d.id)) o.selected = true;
          sel.appendChild(o);
        }
        sel.addEventListener('change', () => {
          pushUndo(); setEpair(s, field.key, sel.value);
          rebuildScene(); syncSidebar();
        });
        wrap.appendChild(lab); wrap.appendChild(sel);
        selBox.appendChild(wrap);
      }
    }
  }

  // ── generic epairs (advanced) ─────────────────────────────────────
  const epTitle = document.createElement('h4');
  epTitle.textContent = 'epairs';
  selBox.appendChild(epTitle);
  for (const e of (s.epairs || [])) {
    // Skip epairs that are already shown via the schema
    if (schema && schema.some(f => f.key === e.k)) continue;
    const row = document.createElement('div');
    row.className = 'epair-row';
    const k = document.createElement('input'); k.value = e.k; k.placeholder = 'key';
    const v = document.createElement('input'); v.value = e.v; v.placeholder = 'value';
    k.addEventListener('change', () => { pushUndo(); e.k = k.value; });
    v.addEventListener('change', () => { pushUndo(); e.v = v.value; });
    const rm = document.createElement('button');
    rm.textContent = '×';
    rm.addEventListener('click', () => {
      pushUndo();
      s.epairs = s.epairs.filter(x => x !== e);
      syncSidebar();
    });
    row.appendChild(k); row.appendChild(v); row.appendChild(rm);
    selBox.appendChild(row);
  }
  const addE = document.createElement('button');
  addE.textContent = '+ epair';
  addE.addEventListener('click', () => {
    pushUndo();
    if (!s.epairs) s.epairs = [];
    s.epairs.push({ k: '', v: '' });
    syncSidebar();
  });
  selBox.appendChild(addE);
  const del = document.createElement('button');
  del.textContent = 'delete spawn';
  del.addEventListener('click', () => {
    pushUndo();
    state.spawns = state.spawns.filter(x => x.id !== s.id);
    state.selection = null;
    rebuildScene();
    syncSidebar();
  });
  selBox.appendChild(del);
}

// ── render loop + resize ──────────────────────────────────────────────────────

function animate() {
  requestAnimationFrame(animate);
  orbit.update();
  // Keep the preview box's wireframe edges crisp — already a mesh, no extra.
  renderer.render(scene, camera);
  labelRenderer.render(scene, camera);
}
animate();


// keyboard
window.addEventListener('keydown', ev => {
  if (ev.target.tagName === 'INPUT' || ev.target.tagName === 'SELECT') return;
  if ((ev.ctrlKey || ev.metaKey) && ev.key === 'z') { ev.preventDefault(); doUndo(); }
  else if ((ev.ctrlKey || ev.metaKey) && (ev.key === 'y' || (ev.shiftKey && ev.key === 'Z'))) { ev.preventDefault(); doRedo(); }
  else if (ev.key === 'Delete' || ev.key === 'Backspace') {
    if (state.selection) {
      pushUndo();
      if (state.selection.kind === 'brush') state.brushes = state.brushes.filter(b => b.id !== state.selection.id);
      else state.spawns = state.spawns.filter(s => s.id !== state.selection.id);
      state.selection = null;
      rebuildScene();
      syncSidebar();
    }
  }
});

// initial empty scene
rebuildScene();
syncSidebar();
$('validate-out').textContent = 'click Validate for a client-side check; ./dev map-validate for the server-side parser.';