// SPDX-License-Identifier: MIT
// snap.js — grid snapping + int16 clamp. Pure, no three.js dependency, so the
// headless round-trip check can pull in mapio.js without a DOM.

export const INT16_MAX = 32767;
export const INT16_MIN = -32768;

export function clampInt16(x) {
  if (!Number.isFinite(x)) return 0;
  return Math.max(INT16_MIN, Math.min(INT16_MAX, x));
}

export function snapRound(x, grid) {
  return Math.round(x / grid) * grid;
}

export function snapFloor(x, grid) {
  return Math.floor(x / grid) * grid;
}

export function snapCeil(x, grid) {
  return Math.ceil(x / grid) * grid;
}

// Snap an AABB's mins/maxs so maxs >= mins on every axis. Used after a face
// drag that could otherwise flip the box (maxs < mins).
export function normalizeAABB(mins, maxs) {
  const out = [[0, 0, 0], [0, 0, 0]];
  for (let i = 0; i < 3; i++) {
    out[0][i] = Math.min(mins[i], maxs[i]);
    out[1][i] = Math.max(mins[i], maxs[i]);
  }
  return out;
}