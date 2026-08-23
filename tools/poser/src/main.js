// SPDX-License-Identifier: MIT
//
// main.js — the poser: load a rig, pose it, key it, ship it back to Blender.
//
// Layout mirrors tools/mapmaker: a viewport that fills the window and one
// panel of controls, no build step, no framework, ES modules straight from
// the served directory.

import * as THREE from "../vendor/three.module.min.js";
import { OrbitControls } from "../vendor/addons/controls/OrbitControls.js";
import { TransformControls } from "../vendor/addons/controls/TransformControls.js";
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

const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
$("viewport").appendChild(renderer.domElement);

const scene = new THREE.Scene();
scene.background = new THREE.Color(0x141024);

const camera = new THREE.PerspectiveCamera(42, 1, 0.05, 200);
camera.position.set(3.4, 2.2, 4.6);

const orbit = new OrbitControls(camera, renderer.domElement);
orbit.target.set(0, 1.1, 0);
orbit.enableDamping = true;

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

function resize() {
  const el = $("viewport");
  renderer.setSize(el.clientWidth, el.clientHeight, false);
  camera.aspect = el.clientWidth / Math.max(1, el.clientHeight);
  camera.updateProjectionMatrix();
}
addEventListener("resize", resize);

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

async function loadActions(name) {
  const index = await (await fetch(`data/${name}.index.json`)).json();
  state.actions = {};
  for (const a of index.actions) {
    state.actions[a.name] = await (await fetch(`data/${a.file}`)).json();
  }
  const sel = $("action");
  sel.innerHTML = "";
  for (const n of Object.keys(state.actions)) {
    sel.append(new Option(n, n));
  }
  selectAction(index.actions[0].name);
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
// No server round trip: this is a static page served by python -m http.server,
// exactly like tools/mapmaker, so writing goes through the browser's download
// and the file lands where anim_io.py reads it from.

function save() {
  const a = state.action;
  a.keys.sort((x, y) => x.frame - y.frame);
  a.length = +$("length").value || a.length;
  a.loop = $("loop").checked;
  const blob = new Blob([JSON.stringify(a, null, 1) + "\n"],
                        { type: "application/json" });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = `${state.model.name}.${a.name}.json`;
  link.click();
  URL.revokeObjectURL(url);
  state.dirty = false;
  $("save").classList.remove("dirty");
  status(`saved ${link.download} — drop it in tools/poser/data/ and rebuild`);
}

function status(msg) { $("status").textContent = msg; }

// ── wiring ────────────────────────────────────────────────────────────────

$("action").onchange = (e) => selectAction(e.target.value);
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
  resize();
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
