// SPDX-License-Identifier: MIT
//
// gltf.js — just enough glTF to load one of this repo's rigged models.
//
// ── Why not GLTFLoader ─────────────────────────────────────────────────────
// three.js ships one, and vendoring it would be another ~200 KB of third-party
// code in the tree for a file format we control both ends of. Everything this
// repo consumes is written by nix/blender.nix through one exporter with one
// set of options, so the surface actually in play is small: separate .bin
// buffer, no GLB container, no Draco, no sparse accessors, no textures, no
// morph targets, one skin, TRS nodes.
//
// So this reads exactly that, in about 120 lines, and throws on anything
// outside it rather than silently half-working. If the pipeline ever starts
// emitting something new, this fails loudly at load with the reason.
//
// tools/blender/kilnlib.py's own comment makes the same call about Fast64:
// vendoring a large dependency to talk to a format you already control is a
// cost with no matching benefit.

const COMPONENT = {
  5120: Int8Array, 5121: Uint8Array, 5122: Int16Array,
  5123: Uint16Array, 5125: Uint32Array, 5126: Float32Array,
};
const NCOMP = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT4: 16 };

export async function loadGltf(url) {
  const json = await (await fetch(url)).json();

  if (json.extensionsRequired?.length) {
    throw new Error(`gltf: needs extensions this reader does not have: ` +
                    json.extensionsRequired.join(", "));
  }
  if ((json.buffers || []).length !== 1 || !json.buffers[0].uri) {
    throw new Error("gltf: expected exactly one external .bin buffer");
  }

  const base = url.slice(0, url.lastIndexOf("/") + 1);
  const bin = await (await fetch(base + json.buffers[0].uri)).arrayBuffer();

  const accessor = (i) => {
    const a = json.accessors[i];
    if (a.sparse) throw new Error("gltf: sparse accessors not supported");
    const view = json.bufferViews[a.bufferView];
    const Type = COMPONENT[a.componentType];
    const n = NCOMP[a.type];
    const offset = (view.byteOffset || 0) + (a.byteOffset || 0);
    // An interleaved bufferView needs a strided copy; Blender's exporter
    // writes tightly packed views, so this asserts rather than implements it.
    if (view.byteStride && view.byteStride !== n * Type.BYTES_PER_ELEMENT) {
      throw new Error("gltf: interleaved bufferViews not supported");
    }
    return new Type(bin, offset, a.count * n);
  };

  return { json, bin, accessor };
}

/** The node hierarchy, flattened, with parent links resolved. */
export function readNodes(g) {
  const nodes = g.json.nodes.map((n, i) => ({
    index: i,
    name: n.name || `node${i}`,
    t: n.translation || [0, 0, 0],
    r: n.rotation || [0, 0, 0, 1],
    s: n.scale || [1, 1, 1],
    children: n.children || [],
    parent: -1,
    mesh: n.mesh ?? -1,
    skin: n.skin ?? -1,
  }));
  nodes.forEach((n) => n.children.forEach((c) => { nodes[c].parent = n.index; }));
  return nodes;
}

/** POSITION / NORMAL / COLOR_0 / JOINTS_0 / WEIGHTS_0 + indices. */
export function readMesh(g, meshIndex = 0) {
  const prims = g.json.meshes[meshIndex].primitives;
  if (prims.length !== 1) {
    throw new Error(`gltf: expected 1 primitive, got ${prims.length}`);
  }
  const p = prims[0];
  const at = p.attributes;
  const out = {
    position: g.accessor(at.POSITION),
    normal: at.NORMAL !== undefined ? g.accessor(at.NORMAL) : null,
    color: at.COLOR_0 !== undefined ? g.accessor(at.COLOR_0) : null,
    joints: at.JOINTS_0 !== undefined ? g.accessor(at.JOINTS_0) : null,
    weights: at.WEIGHTS_0 !== undefined ? g.accessor(at.WEIGHTS_0) : null,
    index: p.indices !== undefined ? g.accessor(p.indices) : null,
  };
  // COLOR_0 may be VEC3 or VEC4 and may be normalised integers; three.js
  // wants float RGB. Normalising here keeps the caller from caring.
  if (out.color) {
    const acc = g.json.accessors[at.COLOR_0];
    const n = NCOMP[acc.type];
    const scale = acc.componentType === 5126 ? 1
                : acc.componentType === 5123 ? 1 / 65535 : 1 / 255;
    const rgb = new Float32Array(acc.count * 3);
    for (let i = 0; i < acc.count; i++) {
      rgb[i * 3 + 0] = out.color[i * n + 0] * scale;
      rgb[i * 3 + 1] = out.color[i * n + 1] * scale;
      rgb[i * 3 + 2] = out.color[i * n + 2] * scale;
    }
    out.color = rgb;
  }
  return out;
}

export function readSkin(g, skinIndex = 0) {
  const s = g.json.skins[skinIndex];
  return {
    joints: s.joints.slice(),
    inverseBind: s.inverseBindMatrices !== undefined
      ? g.accessor(s.inverseBindMatrices) : null,
  };
}

/**
 * The baked animations, as per-node rotation tracks.
 *
 * These are the ground truth the poser checks itself against: they are what
 * Blender actually exported, so if the editor's own forward kinematics
 * reproduces them from the source keyframes in tools/poser/data, the Euler
 * convention is proven rather than assumed. Translation and scale tracks are
 * read too — the walk cycle keys root location.
 */
export function readAnimations(g) {
  return (g.json.animations || []).map((a) => {
    const tracks = [];
    for (const ch of a.channels) {
      const sampler = a.samplers[ch.sampler];
      if (sampler.interpolation && sampler.interpolation !== "LINEAR" &&
          sampler.interpolation !== "STEP") {
        throw new Error(`gltf: ${sampler.interpolation} interpolation ` +
                        `is not supported (animation "${a.name}")`);
      }
      tracks.push({
        node: ch.target.node,
        path: ch.target.path,
        times: g.accessor(sampler.input),
        values: g.accessor(sampler.output),
        step: sampler.interpolation === "STEP",
      });
    }
    const duration = tracks.reduce(
      (d, t) => Math.max(d, t.times[t.times.length - 1] || 0), 0);
    return { name: a.name, tracks, duration };
  });
}
