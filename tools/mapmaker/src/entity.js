// SPDX-License-Identifier: MIT
// entity.js — spawn entity three.js group: an icon + label showing
// classname + origin so a populated map is readable at a glance.
//
// Each FPS entity type gets a distinct color and shape so the level
// designer can tell enemies from pickups from doors at a glance:
//
//   info_player_start  green arrow
//   info_enemy         red arrow
//   info_heavy         dark red arrow (larger)
//   info_health        green cross (small box)
//   info_armor         blue box
//   info_ammo          yellow box
//   info_npc           cyan cylinder
//   info_chest         brown box
//   info_key_door      red tall box
//   info_key_red       red diamond (small)
//   info_switch        teal box
//   info_barrel        orange cylinder
//   info_trigger       wireframe AABB (not drawn at runtime)

import * as THREE from 'three';
import { CSS2DObject } from 'three/addons/renderers/CSS2DRenderer.js';

let nextId = 1;
export function allocId() { return nextId++; }

// ── Entity palette ──────────────────────────────────────────────────────
// Each entry: { color, size, shape, label }
// size = arrow length or box half-extent, in world units.
// shape: 'arrow' | 'box' | 'cylinder' | 'diamond' | 'wireframe'
export const ENTITY_PALETTE = {
  info_player_start: { color: 0x44ff44, size: 24, shape: 'arrow', label: 'START' },
  info_enemy:        { color: 0xff4444, size: 24, shape: 'arrow', label: 'ENEMY' },
  info_heavy:        { color: 0x8b0000, size: 32, shape: 'arrow', label: 'HEAVY' },
  info_health:       { color: 0x00cc44, size: 6,  shape: 'box',   label: 'HP' },
  info_armor:        { color: 0x4488ff, size: 6,  shape: 'box',   label: 'ARM' },
  info_ammo:         { color: 0xffd700, size: 6,  shape: 'box',   label: 'AMMO' },
  info_npc:          { color: 0x88ccff, size: 10, shape: 'cylinder', label: 'NPC' },
  info_chest:        { color: 0x8b4513, size: 12, shape: 'box',   label: 'CHEST' },
  info_key_door:     { color: 0xff3333, size: 14, shape: 'box',   label: 'DOOR' },
  info_key_red:      { color: 0xff4444, size: 6,  shape: 'diamond', label: 'KEY' },
  info_switch:       { color: 0x00f5d4, size: 6,  shape: 'box',   label: 'SW' },
  info_barrel:       { color: 0xff8800, size: 8,  shape: 'cylinder', label: 'BARREL' },
  info_trigger:      { color: 0xff00ff, size: 0,  shape: 'wireframe', label: 'TRIG' },
};

export const KNOWN_CLASSNAMES = Object.keys(ENTITY_PALETTE);

function paletteEntry(classname) {
  return ENTITY_PALETTE[classname] || { color: 0xff5555, size: 24, shape: 'arrow', label: classname };
}

function makeIcon(entry, selected) {
  const color = selected ? 0xffaa00 : entry.color;
  const s = entry.size;

  if (entry.shape === 'arrow') {
    return new THREE.ArrowHelper(
      new THREE.Vector3(0, 0, 1), new THREE.Vector3(0, 0, 0),
      s, color, s * 0.33, s * 0.2,
    );
  }

  if (entry.shape === 'box') {
    const geo = new THREE.BoxGeometry(s * 2, s * 2, s * 2);
    const mat = new THREE.MeshStandardMaterial({
      color, transparent: true, opacity: 0.7,
      emissive: color, emissiveIntensity: 0.3,
    });
    return new THREE.Mesh(geo, mat);
  }

  if (entry.shape === 'cylinder') {
    const geo = new THREE.CylinderGeometry(s, s, s * 2, 8);
    const mat = new THREE.MeshStandardMaterial({
      color, transparent: true, opacity: 0.7,
      emissive: color, emissiveIntensity: 0.3,
    });
    return new THREE.Mesh(geo, mat);
  }

  if (entry.shape === 'diamond') {
    const geo = new THREE.OctahedronGeometry(s, 0);
    const mat = new THREE.MeshStandardMaterial({
      color, transparent: true, opacity: 0.8,
      emissive: color, emissiveIntensity: 0.4,
    });
    return new THREE.Mesh(geo, mat);
  }

  if (entry.shape === 'wireframe') {
    // Placeholder — the actual wireframe is built from mins/maxs epairs
    // in syncSpawnGroup. Return a small marker so the entity is pickable.
    const geo = new THREE.SphereGeometry(4, 8, 6);
    const mat = new THREE.MeshBasicMaterial({
      color, wireframe: true, transparent: true, opacity: 0.5,
    });
    return new THREE.Mesh(geo, mat);
  }

  // Fallback: arrow
  return new THREE.ArrowHelper(
    new THREE.Vector3(0, 0, 1), new THREE.Vector3(0, 0, 0),
    s, color, s * 0.33, s * 0.2,
  );
}

export function makeSpawnGroup(spawn, selected) {
  const group = new THREE.Group();
  group.userData.kind = 'spawn';
  group.userData.id = spawn.id;

  const entry = paletteEntry(spawn.classname);
  const icon = makeIcon(entry, selected);
  group.add(icon);
  group.userData.icon = icon;

  const labelEl = document.createElement('div');
  labelEl.className = 'spawn-label';
  labelEl.textContent = entry.label;
  labelEl.style.color = '#' + entry.color.toString(16).padStart(6, '0');
  const label = new CSS2DObject(labelEl);
  label.position.set(0, entry.size + 4, 0);
  group.add(label);
  group.userData.label = labelEl;

  // Trigger volume wireframe (for info_trigger with mins/maxs epairs).
  let triggerBox = null;
  for (const e of (spawn.epairs || [])) {
    if (e.k === 'mins' || e.k === 'maxs') {
      // Will be fully set up in syncSpawnGroup
      if (!triggerBox) {
        const geo = new THREE.BoxGeometry(1, 1, 1);
        const edges = new THREE.EdgesGeometry(geo);
        const mat = new THREE.LineBasicMaterial({
          color: 0xff00ff, transparent: true, opacity: 0.4,
        });
        triggerBox = new THREE.LineSegments(edges, mat);
        triggerBox.visible = false;
        group.add(triggerBox);
        group.userData.triggerBox = triggerBox;
        geo.dispose(); edges.dispose();
      }
    }
  }

  return group;
}

function parseVec3(str) {
  if (!str) return null;
  const parts = str.split(/\s+/).map(parseFloat);
  if (parts.length !== 3 || parts.some(isNaN)) return null;
  return parts;
}

export function syncSpawnGroup(group, spawn, selected) {
  group.position.set(spawn.origin[0], spawn.origin[1], spawn.origin[2]);
  const rad = (spawn.angle || 0) * Math.PI / 180;
  group.rotation.set(0, -rad, 0);

  const entry = paletteEntry(spawn.classname);

  // Update icon color for selection state
  if (group.userData.icon) {
    const icon = group.userData.icon;
    if (icon.setColor) {
      icon.setColor(selected ? 0xffaa00 : entry.color);
    } else if (icon.material) {
      icon.material.color.setHex(selected ? 0xffaa00 : entry.color);
      if (icon.material.emissive) {
        icon.material.emissive.setHex(selected ? 0xffaa00 : entry.color);
      }
    }
  }

  group.userData.label.textContent = entry.label;
  group.userData.label.style.color = '#' + entry.color.toString(16).padStart(6, '0');

  // Update trigger volume wireframe from mins/maxs epairs
  const tb = group.userData.triggerBox;
  if (tb) {
    let mins = null, maxs = null;
    for (const e of (spawn.epairs || [])) {
      if (e.k === 'mins') mins = parseVec3(e.v);
      if (e.k === 'maxs') maxs = parseVec3(e.v);
    }
    if (mins && maxs) {
      const w = maxs[0] - mins[0];
      const h = maxs[1] - mins[1];
      const d = maxs[2] - mins[2];
      const cx = (mins[0] + maxs[0]) / 2;
      const cy = (mins[1] + maxs[1]) / 2;
      const cz = (mins[2] + maxs[2]) / 2;
      // Position relative to the spawn group (which is at spawn.origin)
      tb.position.set(cx - spawn.origin[0], cy - spawn.origin[1], cz - spawn.origin[2]);
      tb.scale.set(Math.max(0.001, w), Math.max(0.001, h), Math.max(0.001, d));
      tb.visible = true;
    } else {
      tb.visible = false;
    }
  }
}