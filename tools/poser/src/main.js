// SPDX-License-Identifier: MIT
//
// main.js — the poser: load a rig, pose it, key it, ship it back to Blender.
//
// Layout mirrors tools/mapmaker: a viewport that fills the window and one
// panel of controls, no build step, no framework, ES modules straight from
// the served directory.

import * as THREE from "three";
import { TransformControls } from "three/addons/controls/TransformControls.js";
import { createViewport } from "webcommon/viewport.js";
import { StudioFile, describeSaveError, download, saveOrAsk, studioParams } from "webcommon/io.js";
import { loadGltf, readNodes, readMesh, readSkin, readAnimations } from "./gltf.js";
import { poseToQuat, quatToPose, applyAction, actionBones, boneAt, verify }
  from "./pose.js";

const $ = (id) => document.getElementById(id);

const state = {
  model: null,          // { nodes, bones, nodeOfBone, skinned, restPose }
  actions: {},          // name -> source action (sparse keys)
  baked: {},            // name -> glTF-baked animation
  action: null,         // the one being edited
  frame: 0,
  playing: false,
  selected: null,       // bone name
  dirty: false,
};

// ── scene ─────────────────────────────────────────────────────────────────

const view = createViewport($("viewport"), {
  background: 0x141024, fov: 42, near: 0.05, far: 200,
  eye: [3.4, 2.2, 4.6], target: [0, 1.1, 0], maxPixelRatio: 2, updateStyle: false,
});
const { renderer, scene, camera, orbit } = view;

const gizmo = new TransformControls(camera, renderer.domElement);
gizmo.setMode("rotate");
gizmo.setSpace("local");   // bone-local: the space goblin.py's numbers are in
gizmo.setSize(0.55);
scene.add(gizmo.getHelper ? gizmo.getHelper() : gizmo);
gizmo.addEventListener("dragging-changed", (e) => { orbit.enabled = !e.value; });
gizmo.addEventListener("objectChange", onGizmoMoved);

// Flat, N64-ish: one directional light plus a fill, no shadows, vertex colours
// carrying the actual look.
scene.add(new THREE.AmbientLight(0xffffff, 1.05));
const key = new THREE.DirectionalLight(0xffffff, 1.5);
key.position.set(2, 4, 3);
scene.add(key);

const grid = new THREE.GridHelper(8, 16, 0x4a3f78, 0x2a2444);
scene.add(grid);


// ── loading ───────────────────────────────────────────────────────────────

async function loadModel(name) {
  status(`loading ${name}…`);
  const g = await loadGltf(`data/${name}.gltf`);
  const nodes = readNodes(g);
  const mesh = readMesh(g);
  const skin = readSkin(g);
  const anims = readAnimations(g);

  // Build three.js Bones mirroring the glTF node hierarchy. Only the joints
  // and their ancestors matter; the mesh node carries no transform here.
  const objs = nodes.map((n) => {
    const b = new THREE.Bone();
    b.name = n.name;
    b.position.fromArray(n.t);
    b.quaternion.fromArray(n.r);
    b.scale.fromArray(n.s);
    return b;
  });
  nodes.forEach((n) => n.children.forEach((c) => objs[n.index].add(objs[c])));

  const root = new THREE.Group();
  nodes.filter((n) => n.parent === -1).forEach((n) => root.add(objs[n.index]));

  const geo = new THREE.BufferGeometry();
  geo.setAttribute("position", new THREE.BufferAttribute(mesh.position, 3));
  if (mesh.normal) geo.setAttribute("normal", new THREE.BufferAttribute(mesh.normal, 3));
  if (mesh.color) geo.setAttribute("color", new THREE.BufferAttribute(mesh.color, 3));
  if (mesh.index) geo.setIndex(new THREE.BufferAttribute(mesh.index, 1));
  if (!mesh.normal) geo.computeVertexNormals();

  let object;
  if (mesh.joints && mesh.weights) {
    geo.setAttribute("skinIndex", new THREE.BufferAttribute(mesh.joints, 4));
    geo.setAttribute("skinWeight", new THREE.BufferAttribute(mesh.weights, 4));
    const bones = skin.joints.map((j) => objs[j]);
    const inverses = [];
    for (let i = 0; i < bones.length; i++) {
      const m = new THREE.Matrix4();
      if (skin.inverseBind) m.fromArray(skin.inverseBind, i * 16);
      inverses.push(m);
    }
    object = new THREE.SkinnedMesh(
      geo, new THREE.MeshLambertMaterial({ vertexColors: !!mesh.color }));
    object.add(root);
    object.bind(new THREE.Skeleton(bones, inverses));
  } else {
    object = new THREE.Mesh(
      geo, new THREE.MeshLambertMaterial({ vertexColors: !!mesh.color }));
    object.add(root);
  }

  if (state.model) scene.remove(state.model.object);
  scene.add(object);

  // Bone name -> { node, rest }. `rest` is the node's authored rotation,
  // which IS the bone's rest-local transform — see pose.js.
  const bones = {}, nodeOfBone = {};
  for (const j of skin.joints) {
    const n = nodes[j];
    bones[n.name] = { node: objs[j], rest: new THREE.Quaternion().fromArray(n.r) };
    nodeOfBone[n.name] = j;
  }

  state.model = { name, nodes, objs, bones, nodeOfBone, object };
  state.baked = Object.fromEntries(anims.map((a) => [a.name, a]));

  buildBoneList();
  status(`${name}: ${skin.joints.length} bones, ` +
         `${mesh.position.length / 3} verts, ${anims.length} baked actions`);
}

// Inside Kiln Studio each action is opened as a studio file, so its save is a
// compare-and-swap against the version loaded here and the editor holds the
// action's advisory lock while it is selected.
const studio = studioParams();
let actionFiles = {};

function holdAction(name) {
  for (const f of Object.values(actionFiles)) f.release();
  const f = actionFiles[name];
  if (f) f.hold((err) => { if (err) status(`${err.message} — saving will ask before taking it over`); });
}

async function loadActions(name) {
  const index = await (await fetch(`data/${name}.index.json`)).json();
  state.actions = {};
  for (const f of Object.values(actionFiles)) f.release();
  actionFiles = {};
  for (const a of index.actions) {
    if (studio) {
      const f = new StudioFile(`tools/poser/data/${a.file}`);
      state.actions[a.name] = JSON.parse((await f.open()).text);
      actionFiles[a.name] = f;
    } else {
      state.actions[a.name] = await (await fetch(`data/${a.file}`)).json();
    }
  }
  const sel = $("action");
  sel.innerHTML = "";
  for (const n of Object.keys(state.actions)) {
    sel.append(new Option(n, n));
  }
  selectAction(index.actions[0].name);
  holdAction(index.actions[0].name);
}

// ── editing ───────────────────────────────────────────────────────────────

function selectAction(name) {
  state.action = structuredClone(state.actions[name]);
  state.frame = 0;
  $("action").value = name;
  $("scrub").max = state.action.length;
  $("loop").checked = !!state.action.loop;
  $("length").value = state.action.length;
  buildKeyList();
  runVerify();
  refresh();
}

/** The key at the current frame, if the playhead is exactly on one. */
function keyAtPlayhead() {
  return state.action.keys.find((k) => k.frame === Math.round(state.frame));
}

function restPose() {
  // goblin.py's `_bake` falls back to the action's own `rest` for bones a key
  // omits. The JSON does not carry that separately, so frame 0 stands in —
  // which is what every action here uses as its base pose anyway.
  return state.action.keys.length ? state.action.keys[0].bones : {};
}

function refresh() {
  if (!state.model || !state.action) return;
  applyAction(state.model.bones, state.action, state.frame, restPose());
  $("scrub").value = state.frame;
  $("frameNum").textContent = Math.round(state.frame);
  const k = keyAtPlayhead();
  $("onKey").textContent = k ? `key @ ${k.frame} (${k.ease || "inout"})` : "—";
  $("setKey").textContent = k ? "update key" : "add key";
  updateNumericReadout();
  for (const el of document.querySelectorAll("#keys li")) {
    el.classList.toggle("cur", +el.dataset.frame === Math.round(state.frame));
  }
}

function buildBoneList() {
  const ul = $("bones");
  ul.innerHTML = "";
  for (const name of Object.keys(state.model.bones)) {
    const li = document.createElement("li");
    li.textContent = name;
    li.dataset.bone = name;
    li.onclick = () => selectBone(name);
    ul.append(li);
  }
}

function selectBone(name) {
  state.selected = name;
  for (const li of document.querySelectorAll("#bones li")) {
    li.classList.toggle("cur", li.dataset.bone === name);
  }
  gizmo.attach(state.model.bones[name].node);
  updateNumericReadout();
}

function updateNumericReadout() {
  const n = $("rot");
  if (!state.selected) { n.textContent = "select a bone"; return; }
  const b = state.model.bones[state.selected];
  const deg = quatToPose(b.rest, b.node.quaternion);
  n.textContent = `${state.selected}  ` +
    deg.map((d) => (d >= 0 ? "+" : "") + d.toFixed(1)).join("  ");
  $("py").value =
    `"${state.selected}": (${deg.map((d) => Math.round(d)).join(", ")}),`;
}

/**
 * The gizmo moved a bone. Write it into the key under the playhead — or
 * create one. Editing a frame that is not a key would otherwise be silently
 * discarded on the next refresh, which is the single most annoying thing an
 * animation tool can do.
 */
function onGizmoMoved() {
  if (!state.selected || !state.action) return;
  const b = state.model.bones[state.selected];
  const deg = quatToPose(b.rest, b.node.quaternion).map((d) => +d.toFixed(2));
  let k = keyAtPlayhead();
  if (!k) {
    k = addKey(Math.round(state.frame));
  }
  k.bones[state.selected] = deg;
  state.dirty = true;
  markDirty();
  updateNumericReadout();
}

function addKey(frame) {
  // A new key starts from the pose currently on screen, so inserting one
  // never moves anything.
  const bones = {};
  for (const name of actionBones(state.action)) {
    bones[name] = boneAt(state.action, name, frame, restPose())
      .map((d) => +d.toFixed(2));
  }
  const k = { frame, ease: "inout", bones };
  state.action.keys.push(k);
  state.action.keys.sort((a, b2) => a.frame - b2.frame);
  buildKeyList();
  return k;
}

function deleteKey(frame) {
  if (state.action.keys.length <= 2) { status("keep at least two keys"); return; }
  state.action.keys = state.action.keys.filter((k) => k.frame !== frame);
  buildKeyList();
  markDirty();
  refresh();
}

function buildKeyList() {
  const ul = $("keys");
  ul.innerHTML = "";
  for (const k of state.action.keys) {
    const li = document.createElement("li");
    li.dataset.frame = k.frame;
    const sel = document.createElement("select");
    for (const m of ["inout", "out", "in", "over", "linear"]) {
      sel.append(new Option(m, m));
    }
    sel.value = k.ease || "inout";
    sel.onchange = () => { k.ease = sel.value; markDirty(); refresh(); };
    sel.onclick = (e) => e.stopPropagation();
    const label = document.createElement("span");
    label.textContent = `f${k.frame}`;
    const del = document.createElement("button");
    del.textContent = "×";
    del.onclick = (e) => { e.stopPropagation(); deleteKey(k.frame); };
    li.append(label, sel, del);
    li.onclick = () => { state.frame = k.frame; refresh(); };
    ul.append(li);
  }
}

function markDirty() {
  state.dirty = true;
  $("save").classList.add("dirty");
  $("verify").textContent = "edited — re-verify after saving + rebuilding";
  $("verify").className = "warn";
}

// ── verification ──────────────────────────────────────────────────────────

function runVerify() {
  const el = $("verify");
  if (!state.model || !state.action) return;
  const baked = state.baked[state.action.name];
  const r = verify(state.model.bones, state.action, baked,
                   state.model.nodeOfBone, restPose());
  if (!r.ok && r.reason) { el.textContent = r.reason; el.className = "warn"; return; }
  el.textContent = `vs Blender's own bake: worst ${r.worst.toFixed(2)}° ` +
                   `(${r.bone} @ f${Math.round(r.frame)})`;
  el.className = r.ok ? "ok" : "bad";
}

// ── save ──────────────────────────────────────────────────────────────────
// Standalone this is a static page with nothing to write to, so saving is a
// download of the file anim_io.py reads. Inside Kiln Studio it writes
// tools/poser/data/ in place (tools/webcommon/io.js).

async function save() {
  const a = state.action;
  a.keys.sort((x, y) => x.frame - y.frame);
  a.length = +$("length").value || a.length;
  a.loop = $("loop").checked;
  const text = JSON.stringify(a, null, 1) + "\n";
  const filename = `${state.model.name}.${a.name}.json`;
  const file = actionFiles[a.name];
  if (file) {
    try {
      await saveOrAsk(file, text);
      status(`saved ${file.path} — rebuild the model to bake it`);
    } catch (e) {
      status(describeSaveError(e));
      return;
    }
  } else {
    download(filename, text, "application/json");
    status(`saved ${filename} — drop it in tools/poser/data/ and rebuild`);
  }
  state.dirty = false;
  $("save").classList.remove("dirty");
}

function status(msg) { $("status").textContent = msg; }

// ── wiring ────────────────────────────────────────────────────────────────

$("action").onchange = (e) => { selectAction(e.target.value); holdAction(e.target.value); };
$("scrub").oninput = (e) => { state.frame = +e.target.value; refresh(); };
$("play").onclick = () => {
  state.playing = !state.playing;
  $("play").textContent = state.playing ? "pause" : "play";
};
$("setKey").onclick = () => {
  const f = Math.round(state.frame);
  if (!keyAtPlayhead()) addKey(f);
  markDirty();
  refresh();
};
$("save").onclick = save;
$("reverify").onclick = runVerify;
$("length").onchange = () => {
  state.action.length = +$("length").value;
  $("scrub").max = state.action.length;
  refresh();
};
$("loop").onchange = () => { state.action.loop = $("loop").checked; };
$("gizmoMode").onchange = (e) => gizmo.setMode(e.target.value);
$("model").onchange = async (e) => {
  await loadModel(e.target.value);
  await loadActions(e.target.value);
};

addEventListener("keydown", (e) => {
  if (e.target.tagName === "INPUT" || e.target.tagName === "SELECT") return;
  if (e.key === " ") { $("play").click(); e.preventDefault(); }
  if (e.key === "ArrowRight") { state.frame = Math.min(state.action.length, state.frame + 1); refresh(); }
  if (e.key === "ArrowLeft") { state.frame = Math.max(0, state.frame - 1); refresh(); }
  if (e.key === "k") $("setKey").click();
});

// ── run ───────────────────────────────────────────────────────────────────

let last = performance.now();
function tick(now) {
  const dt = (now - last) / 1000;
  last = now;
  if (state.playing && state.action) {
    // 30 fps playback: the ROM's target, so the preview lies about timing by
    // as little as possible.
    state.frame += dt * 30;
    if (state.frame > state.action.length) {
      state.frame = state.action.loop ? 0 : state.action.length;
      if (!state.action.loop) { state.playing = false; $("play").textContent = "play"; }
    }
    refresh();
  }
  orbit.update();
  renderer.render(scene, camera);
  requestAnimationFrame(tick);
}

(async function boot() {
  view.resize();
  if (studio && studio.model && [...$("model").options].some((o) => o.value === studio.model)) {
    $("model").value = studio.model;
  }
  requestAnimationFrame(tick);
  try {
    const name = $("model").value;
    await loadModel(name);
    await loadActions(name);
  } catch (err) {
    status(`load failed: ${err.message}`);
    console.error(err);
  }
})();
