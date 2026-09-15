// SPDX-License-Identifier: MIT
//
// viewport.js — the three.js viewport the map maker and the poser share: a
// renderer filling its container, a scene, a perspective camera on damped
// orbit controls, and resizing that keeps all of it — and any overlay renderer,
// such as the map maker's CSS2D labels — matched to the container. Each editor
// used to carry its own copy. Lights, grids and gizmos stay with the editors,
// because that is where they differ.

import * as THREE from "three";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";

export function createViewport(container, {
  background = 0x000000, fov = 50, near = 0.1, far = 1000,
  eye = [0, 0, 10], target = [0, 0, 0], damping = 0.05,
  maxPixelRatio = Infinity, updateStyle = true,
} = {}) {
  const renderer = new THREE.WebGLRenderer({ antialias: true });
  renderer.setPixelRatio(Math.min(globalThis.devicePixelRatio || 1, maxPixelRatio));
  container.appendChild(renderer.domElement);

  const scene = new THREE.Scene();
  scene.background = new THREE.Color(background);

  const camera = new THREE.PerspectiveCamera(fov, 1, near, far);
  camera.position.set(...eye);

  const orbit = new OrbitControls(camera, renderer.domElement);
  orbit.enableDamping = true;
  orbit.dampingFactor = damping;
  orbit.target.set(...target);

  const overlays = [];
  function resize() {
    const w = container.clientWidth;
    const h = Math.max(1, container.clientHeight);
    renderer.setSize(w, h, updateStyle);
    for (const o of overlays) o.setSize(w, h);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
  }
  globalThis.addEventListener("resize", resize);
  resize();

  return {
    renderer, scene, camera, orbit, resize,
    overlay(r) {
      overlays.push(r);
      resize();
      return r;
    },
  };
}
