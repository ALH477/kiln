// SPDX-License-Identifier: MIT
// brush.js — AABB brush three.js mesh creation + sync. Each brush is a
// BoxGeometry mesh sized to (maxs - mins) and positioned at the centre; a
// CSS2DObject label is attached for readability at high brush counts.

import * as THREE from 'three';

let nextId = 1;
export function allocId() { return nextId++; }

export function makeBrushMesh(brush, selected) {
  const geom = new THREE.BoxGeometry(1, 1, 1);
  const mat = new THREE.MeshStandardMaterial({
    color: selected ? 0xffcc33 : 0x88a8c8,
    transparent: true,
    opacity: 0.45,
    roughness: 0.7,
    metalness: 0.0,
    depthWrite: false,
  });
  const mesh = new THREE.Mesh(geom, mat);
  mesh.userData.kind = 'brush';
  mesh.userData.id = brush.id;
  // Wireframe overlay so the AABB edges read clearly at low opacity.
  const edges = new THREE.LineSegments(
    new THREE.EdgesGeometry(geom),
    new THREE.LineBasicMaterial({ color: selected ? 0xffaa00 : 0x223344 })
  );
  mesh.add(edges);
  mesh.userData.edges = edges;
  return mesh;
}

export function syncBrushMesh(mesh, brush, selected) {
  const sx = brush.maxs[0] - brush.mins[0];
  const sy = brush.maxs[1] - brush.mins[1];
  const sz = brush.maxs[2] - brush.mins[2];
  mesh.scale.set(sx || 0.001, sy || 0.001, sz || 0.001);
  mesh.position.set(
    (brush.mins[0] + brush.maxs[0]) / 2,
    (brush.mins[1] + brush.maxs[1]) / 2,
    (brush.mins[2] + brush.maxs[2]) / 2,
  );
  mesh.material.color.setHex(selected ? 0xffcc33 : 0x88a8c8);
  mesh.userData.edges.material.color.setHex(selected ? 0xffaa00 : 0x223344);
}