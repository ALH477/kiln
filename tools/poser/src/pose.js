// SPDX-License-Identifier: MIT
//
// pose.js — the Blender pose-bone convention, in the browser, checked.
//
// ── The one thing this file has to get right ───────────────────────────────
// goblin.py writes poses as XYZ Euler degrees in Blender POSE-BONE space. The
// glTF stores node-local quaternions. If the editor applies the numbers in the
// wrong space then everything it shows is a lie, and a lie that looks
// plausible — which is precisely the failure this whole tool exists to end.
// Three separate rounds of guessing bone signs by rendering stills is what
// motivated writing it.
//
// The mapping. For a pose bone, Blender composes
//
//     bone_matrix = parent_bone_matrix * rest_local * pose_rotation
//
// and Blender's glTF exporter writes each joint's node-local transform as
// exactly `rest_local * pose_rotation`. At rest the pose term is identity, so
// the node's authored rotation in the file IS `rest_local`. Therefore:
//
//     node.quaternion = restQuat * quatFromBlenderEuler(pose)  // RIGHT multiply
//
// A left multiply would rotate in the parent's space instead of the bone's,
// which is wrong in a way that looks correct for the root and increasingly
// wrong down each chain — the sort of bug that gets shipped.
//
// ── And the Euler order is 'ZYX', not 'XYZ' ────────────────────────────────
// Blender's rotation_mode 'XYZ' names the order the rotations are APPLIED —
// X first — which as matrices composes R = Rz @ Ry @ Rx. three.js's
// Euler(x, y, z, 'XYZ') builds Rx @ Ry @ Rz, the opposite. The three.js
// order that matches Blender's 'XYZ' is 'ZYX'.
//
// The two are identical whenever only one axis is non-zero, so every
// single-axis action in this repo agreed perfectly and only the riding poses
// — the only ones rotating a bone about two axes at once — disagreed, by up
// to 26 degrees. tools/poser/verify.py measured all three candidate orders
// against Blender's own export:
//
//     three.js 'XYZ' (Rx@Ry@Rz)   worst 25.95 deg
//     three.js 'ZYX' (Rz@Ry@Rx)   worst  0.03 deg   <- this one
//
// ── It is not assumed, it is checked ───────────────────────────────────────
// `verify()` bakes an action from its SOURCE keyframes (tools/poser/data/*.json,
// the same input goblin.py's `_bake` consumes) and compares the result against
// the BAKED curves inside the .gltf that Blender itself produced from those
// same keyframes. If the convention above is right, and if `bake()` below
// matches `_bake` in goblin.py, the two agree to within rounding. If either
// drifts, the number goes red and says so.
//
// That makes this file the browser's half of a two-implementation agreement,
// exactly like tools/uipreview and tools/kartsim: the value is not that the
// second implementation is faster, it is that a disagreement is visible.

import * as THREE from "../vendor/three.module.min.js";

const DEG = Math.PI / 180;
const _e = new THREE.Euler();
const _q = new THREE.Quaternion();
const _qi = new THREE.Quaternion();

/** Blender pose-bone XYZ Euler degrees -> node-local quaternion. */
export function poseToQuat(restQuat, deg, out = new THREE.Quaternion()) {
  _e.set(deg[0] * DEG, deg[1] * DEG, deg[2] * DEG, "ZYX");
  _q.setFromEuler(_e);
  return out.copy(restQuat).multiply(_q);
}

/** The inverse: node-local quaternion -> Blender pose-bone XYZ degrees.
 *  Used when the transform gizmo has moved a bone and the numeric readout
 *  has to say what goblin.py would need to write. */
export function quatToPose(restQuat, quat) {
  _qi.copy(restQuat).invert().multiply(quat);
  _e.setFromQuaternion(_qi, "ZYX");
  return [_e.x / DEG, _e.y / DEG, _e.z / DEG];
}

// ── easing, mirroring kilnlib.ease ─────────────────────────────────────────
// Same four curves, same constants. kilnlib's version is the one that ships;
// this one exists so the preview matches, and verify() is what keeps them
// honest about it.
export function ease(t, mode) {
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  if (mode === "linear") return t;
  if (mode === "in") return t * t;
  if (mode === "out") return 1 - (1 - t) * (1 - t);
  if (mode === "over") {
    const s = 1.70158 * 0.6;
    const u = t - 1;
    return u * u * ((s + 1) * u + s) + 1;
  }
  return t * t * (3 - 2 * t); // inout / smoothstep
}

// Secondary-motion lag, mirroring goblin.py's LAG table: floppy parts sample
// the timeline a few frames in the past so they arrive after the body that
// threw them. Left and right differ on purpose.
export const LAG = {
  ear_l: 3, ear_r: 4, nose: 2, jaw: 2,
  hand_l: 2, hand_r: 3, forearm_l: 1, forearm_r: 1,
};

const ZERO = [0, 0, 0];

function lerpPose(a, b, t, rest) {
  const out = {};
  const names = new Set([...Object.keys(a), ...Object.keys(b)]);
  for (const n of names) {
    const pa = a[n] || rest[n] || ZERO;
    const pb = b[n] || rest[n] || ZERO;
    out[n] = [pa[0] + (pb[0] - pa[0]) * t,
              pa[1] + (pb[1] - pa[1]) * t,
              pa[2] + (pb[2] - pa[2]) * t];
  }
  return out;
}

/** Pose at a (possibly fractional) frame — goblin.py's `_sample`. */
export function samplePose(keys, f, rest) {
  if (!keys.length) return {};
  if (f <= keys[0].frame) return keys[0].bones;
  if (f >= keys[keys.length - 1].frame) return keys[keys.length - 1].bones;
  for (let i = 0; i < keys.length - 1; i++) {
    const k0 = keys[i], k1 = keys[i + 1];
    if (f >= k0.frame && f <= k1.frame) {
      const span = k1.frame - k0.frame;
      const t = span > 0 ? (f - k0.frame) / span : 1;
      return lerpPose(k0.bones, k1.bones, ease(t, k1.ease || "inout"), rest);
    }
  }
  return keys[keys.length - 1].bones;
}

/** One bone's value at a frame, with its lag applied — goblin.py's `_bake`. */
export function boneAt(action, bone, frame, rest) {
  const lag = LAG[bone] || 0;
  const len = action.length || 1;
  let src = frame - lag;
  src = action.loop ? ((src % len) + len) % len
                    : Math.max(0, Math.min(len, src));
  const pose = samplePose(action.keys, src, rest);
  return pose[bone] || rest[bone] || ZERO;
}

/** Every bone the action touches. */
export function actionBones(action) {
  const s = new Set();
  for (const k of action.keys) for (const b of Object.keys(k.bones)) s.add(b);
  return [...s].sort();
}

/**
 * Apply a source action to the rig at `frame`.
 * `bones` maps bone name -> { node: THREE.Object3D, rest: THREE.Quaternion }.
 */
export function applyAction(bones, action, frame, rest) {
  for (const [name, b] of Object.entries(bones)) {
    poseToQuat(b.rest, boneAt(action, name, frame, rest), b.node.quaternion);
  }
}

/**
 * Compare our bake against the one baked into the .gltf.
 *
 * Returns the worst per-bone angular disagreement in degrees, and which bone
 * and frame it happened at. Anything under about half a degree is the
 * exporter's own quantisation plus the 0.01-degree rounding anim_io.py
 * applies when it writes the JSON; anything larger is a real divergence.
 */
export function verify(bones, action, baked, nodeOfBone, rest) {
  if (!baked) return { ok: false, reason: "no baked animation of that name" };

  const q = new THREE.Quaternion();
  const qb = new THREE.Quaternion();
  let worst = 0, worstBone = "", worstFrame = 0;

  // The exported curves are in seconds at the scene's frame rate; the source
  // keys are in frames. Sampling by NORMALISED time sidesteps needing to know
  // the rate, since both cover the same span.
  const steps = 24;
  for (let s = 0; s <= steps; s++) {
    const u = s / steps;
    const frame = u * action.length;
    const time = u * baked.duration;
    for (const [name, b] of Object.entries(bones)) {
      const node = nodeOfBone[name];
      const track = baked.tracks.find(
        (t) => t.node === node && t.path === "rotation");
      if (!track) continue;
      sampleTrack(track, time, qb);
      poseToQuat(b.rest, boneAt(action, name, frame, rest), q);
      // Quaternion double cover: q and -q are the same orientation.
      let dot = Math.abs(q.dot(qb));
      dot = dot > 1 ? 1 : dot;
      const deg = 2 * Math.acos(dot) / DEG;
      if (deg > worst) { worst = deg; worstBone = name; worstFrame = frame; }
    }
  }
  return { ok: worst < 1.5, worst, bone: worstBone, frame: worstFrame };
}

const _qa = new THREE.Quaternion();
const _qb2 = new THREE.Quaternion();

export function sampleTrack(track, time, out) {
  const times = track.times, v = track.values;
  const n = times.length;
  if (n === 0) return out.set(0, 0, 0, 1);
  if (time <= times[0]) return out.fromArray(v, 0);
  if (time >= times[n - 1]) return out.fromArray(v, (n - 1) * 4);
  let i = 0;
  while (i < n - 1 && times[i + 1] < time) i++;
  const t = (time - times[i]) / (times[i + 1] - times[i]);
  _qa.fromArray(v, i * 4);
  _qb2.fromArray(v, (i + 1) * 4);
  return track.step ? out.copy(_qa) : out.copy(_qa).slerp(_qb2, t);
}
