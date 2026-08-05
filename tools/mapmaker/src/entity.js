// SPDX-License-Identifier: MPL-2.0
// entity.js — spawn entity three.js group: an arrow helper rotated to `angle`
// (yaw around +Y, engine convention) plus a CSS2DObject label showing
// classname + origin so a populated map is readable at a glance.

import * as THREE from 'three';
import { CSS2DObject } from 'three/addons/renderers/CSS2DRenderer.js';

let nextId = 1;
export function allocId() { return nextId++; }

export function makeSpawnGroup(spawn, selected) {
  const group = new THREE.Group();
  group.userData.kind = 'spawn';
  group.userData.id = spawn.id;
  const arrow = new THREE.ArrowHelper(
    new THREE.Vector3(0, 0, 1),
    new THREE.Vector3(0, 0, 0),
    24,
    selected ? 0xffaa00 : 0xff5555,
    8,
    5,
  );
  group.add(arrow);
  group.userData.arrow = arrow;

  const labelEl = document.createElement('div');
  labelEl.className = 'spawn-label';
  labelEl.textContent = spawn.classname;
  const label = new CSS2DObject(labelEl);
  label.position.set(0, 8, 0);
  group.add(label);
  group.userData.label = labelEl;
  return group;
}

export function syncSpawnGroup(group, spawn, selected) {
  group.position.set(spawn.origin[0], spawn.origin[1], spawn.origin[2]);
  // angle is degrees yaw around +Y. Engine: angle 0 = +Z, 90 = +X, etc.
  // ArrowHelper points +Z by default; rotate -Y by `angle` so 0 -> +Z, 90 -> +X.
  const rad = (spawn.angle || 0) * Math.PI / 180;
  group.rotation.set(0, -rad, 0);
  group.userData.arrow.setColor(selected ? 0xffaa00 : 0xff5555);
  group.userData.label.textContent = spawn.classname;
}