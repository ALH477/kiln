#!/usr/bin/env python3
"""
N64-style Island Generator
Landmark-first procedural world builder producing a low-poly island
with roads, rivers, cliffs, adaptive density and vertex colors.
Exports a grouped Wavefront OBJ.
"""

import numpy as np
from dataclasses import dataclass
from typing import List, Tuple, Dict
from collections import defaultdict
import heapq
from pathlib import Path as FilePath

from scipy.ndimage import (
    gaussian_filter,
    maximum_filter,
    distance_transform_edt,
)
from matplotlib.path import Path as MplPath


# ============================================================
# Data structures
# ============================================================

@dataclass
class Landmark:
    name: str
    kind: str
    pos: Tuple[float, float]      # normalized 0-1
    radius: float
    height_influence: float
    flat_radius: float = 0.0


@dataclass
class NavNode:
    id: str
    pos: Tuple[float, float]
    kind: str


@dataclass
class NavEdge:
    a: str
    b: str
    path: List[Tuple[int, int]]
    kind: str = "road"


# ============================================================
# Noise utilities
# ============================================================

def seeded_rng(seed: int) -> np.random.Generator:
    return np.random.default_rng(seed)


def normalize(a: np.ndarray) -> np.ndarray:
    a = a.astype(np.float64)
    a -= a.min()
    mx = a.max()
    if mx < 1e-8:
        return a
    return a / mx


def value_noise(shape: Tuple[int, int], rng: np.random.Generator, scale: float = 1.0) -> np.ndarray:
    h, w = shape
    ch = max(2, h // 8)
    cw = max(2, w // 8)
    coarse = rng.random((ch, cw))
    # bilinear upsample via zoom
    from scipy.ndimage import zoom
    return zoom(coarse, (h / ch, w / cw), order=1) * scale


def fbm(shape: Tuple[int, int], rng: np.random.Generator,
        octaves: int = 5, persistence: float = 0.5) -> np.ndarray:
    noise = np.zeros(shape, dtype=np.float64)
    amp = 1.0
    for i in range(octaves):
        noise += amp * value_noise(shape, rng, scale=1.0)
        amp *= persistence
        # slightly different seed each octave by advancing rng
    return normalize(noise)


# ============================================================
# 1. Landmarks first
# ============================================================

def place_landmarks(seed: int, shape: Tuple[int, int]) -> List[Landmark]:
    rng = seeded_rng(seed)
    landmarks = []

    # Curated set with light jitter – keeps strong identity while varying
    base = [
        ("central_peak",   "mountain", (0.50, 0.47), 0.23,  0.90, 0.035),
        ("north_ruins",    "ruins",    (0.33, 0.20), 0.09,  0.18, 0.055),
        ("east_village",   "village",  (0.78, 0.55), 0.11,  0.06, 0.075),
        ("south_temple",   "temple",   (0.42, 0.80), 0.10,  0.22, 0.065),
        ("west_cliffs",    "cliff",    (0.16, 0.48), 0.15,  0.45, 0.0),
        ("crater_lake",    "lake",     (0.63, 0.30), 0.085,-0.28, 0.05),
        ("lookout_ridge",  "cliff",    (0.68, 0.68), 0.10,  0.35, 0.0),
    ]

    for name, kind, (nx, ny), rad, h_inf, flat in base:
        jx = rng.uniform(-0.035, 0.035)
        jy = rng.uniform(-0.035, 0.035)
        landmarks.append(Landmark(
            name=name,
            kind=kind,
            pos=(float(np.clip(nx + jx, 0.07, 0.93)),
                 float(np.clip(ny + jy, 0.07, 0.93))),
            radius=rad,
            height_influence=h_inf,
            flat_radius=flat
        ))
    return landmarks


# ============================================================
# 2. Irregular silhouette
# ============================================================

def generate_silhouette_mask(shape: Tuple[int, int], seed: int,
                             n_points: int = 16) -> np.ndarray:
    rng = seeded_rng(seed + 7)
    h, w = shape
    cy, cx = (h - 1) / 2.0, (w - 1) / 2.0

    angles = np.linspace(0, 2 * np.pi, n_points, endpoint=False)
    # larger, more varied radii so the island fills most of the map
    base_r = 0.55 + 0.18 * rng.random(n_points)
    noise = 0.09 * np.sin(2.2 * angles + rng.random() * 5) + 0.06 * rng.random(n_points)
    radii = np.clip(base_r + noise, 0.32, 0.92)

    px = cx + radii * np.cos(angles) * (w * 0.48)
    py = cy + radii * np.sin(angles) * (h * 0.48)
    px = np.append(px, px[0])
    py = np.append(py, py[0])

    poly = MplPath(np.column_stack([px, py]))
    yy, xx = np.mgrid[:h, :w]
    points = np.vstack((xx.ravel(), yy.ravel())).T
    inside = poly.contains_points(points).reshape(h, w).astype(np.float64)

    # soft but still substantial coastal falloff
    dist = distance_transform_edt(inside)
    # scale falloff relative to resolution so larger maps keep solid land
    falloff = max(4.0, min(h, w) * 0.09)
    mask = np.clip(dist / falloff, 0.0, 1.0)
    # boost interior so most of the island is solid land
    mask = np.clip(mask * 1.35, 0.0, 1.0)
    return mask


# ============================================================
# 3. Landmark influence + platforms
# ============================================================

def apply_landmarks(height: np.ndarray, landmarks: List[Landmark],
                    shape: Tuple[int, int]) -> np.ndarray:
    h, w = shape
    yy, xx = np.ogrid[:h, :w]

    for lm in landmarks:
        cy = lm.pos[1] * (h - 1)
        cx = lm.pos[0] * (w - 1)
        dist = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
        norm_dist = dist / (max(h, w) * lm.radius + 1e-6)
        influence = np.exp(-norm_dist ** 2 * 2.8) * lm.height_influence

        if lm.kind == "lake":
            height = height - np.abs(influence)
        elif lm.flat_radius > 0.0:
            # explicit flat gameplay platform
            flat_norm = dist / (max(h, w) * lm.flat_radius + 1e-6)
            platform = np.clip(1.0 - flat_norm, 0.0, 1.0)
            # sample a representative height near the center
            cy_i, cx_i = int(np.clip(cy, 0, h-1)), int(np.clip(cx, 0, w-1))
            target = height[cy_i, cx_i] * 0.65 + 0.28
            height = height * (1.0 - platform * 0.88) + target * platform * 0.88
            height = height + influence * (1.0 - platform)
        else:
            height = height + influence

    return np.clip(height, 0.0, 1.0)


# ============================================================
# 4. Flow accumulation + rivers
# ============================================================

def compute_flow_accumulation(height: np.ndarray, iterations: int = 10) -> np.ndarray:
    acc = np.ones_like(height, dtype=np.float64)
    for _ in range(iterations):
        # simple multi-direction diffusion biased downhill
        gy, gx = np.gradient(-height)
        # shift contributions
        acc[1:, :]  += acc[:-1, :] * 0.22
        acc[:-1, :] += acc[1:, :] * 0.22
        acc[:, 1:]  += acc[:, :-1] * 0.22
        acc[:, :-1] += acc[:, 1:] * 0.22
        acc *= 0.82
    return normalize(acc)


def carve_from_flow(height: np.ndarray, flow: np.ndarray,
                    strength: float = 0.105) -> Tuple[np.ndarray, np.ndarray]:
    river = (flow > np.percentile(flow, 87)).astype(np.float64)
    river = maximum_filter(river, size=2)
    river = gaussian_filter(river, sigma=0.75)
    river = np.clip(river, 0.0, 1.0)
    carved = height - river * strength * (0.55 + 0.45 * flow)
    return np.clip(carved, 0.0, 1.0), river


# ============================================================
# 5. Navigation graph + road carving
# ============================================================

def build_navigation_graph(
    landmarks: List[Landmark],
    height: np.ndarray,
    mask: np.ndarray,
    seed: int
) -> Tuple[List[NavNode], List[NavEdge], np.ndarray, np.ndarray]:
    h, w = height.shape
    nodes = [NavNode(id=lm.name, pos=lm.pos, kind=lm.kind) for lm in landmarks]
    positions = {n.id: n.pos for n in nodes}
    edges: List[NavEdge] = []
    road_mask = np.zeros_like(height)

    def to_grid(nx: float, ny: float) -> Tuple[int, int]:
        return int(np.clip(ny * (h - 1), 0, h-1)), int(np.clip(nx * (w - 1), 0, w-1))

    def heuristic(a, b):
        return np.hypot(a[0] - b[0], a[1] - b[1])

    def astar(start: Tuple[int, int], goal: Tuple[int, int]) -> List[Tuple[int, int]]:
        open_set = []
        heapq.heappush(open_set, (0.0, start))
        came_from = {}
        g_score = defaultdict(lambda: 1e18)
        g_score[start] = 0.0
        closed = set()

        while open_set:
            _, current = heapq.heappop(open_set)
            if current in closed:
                continue
            closed.add(current)
            if current == goal:
                path = []
                while current in came_from:
                    path.append(current)
                    current = came_from[current]
                path.append(start)
                return path[::-1]

            cy, cx = current
            for dy, dx in [(-1,0),(1,0),(0,-1),(0,1),(-1,-1),(-1,1),(1,-1),(1,1)]:
                ny, nx = cy + dy, cx + dx
                if not (0 <= ny < h and 0 <= nx < w):
                    continue
                if mask[ny, nx] < 0.12:          # more permissive
                    continue
                slope_pen = abs(height[ny, nx] - height[cy, cx]) * 11.0
                tentative = g_score[current] + np.hypot(dy, dx) + slope_pen
                key = (ny, nx)
                if tentative < g_score[key]:
                    came_from[key] = current
                    g_score[key] = tentative
                    f = tentative + heuristic(key, goal)
                    heapq.heappush(open_set, (f, key))
        return []

    def straight_fallback(start, goal):
        """Simple linear interpolation fallback when A* fails."""
        path = []
        steps = max(abs(goal[0]-start[0]), abs(goal[1]-start[1]), 1)
        for i in range(steps + 1):
            t = i / steps
            y = int(round(start[0] + t * (goal[0] - start[0])))
            x = int(round(start[1] + t * (goal[1] - start[1])))
            path.append((y, x))
        return path

    ids = [n.id for n in nodes]
    used = set()
    for a in ids:
        dists = []
        for b in ids:
            if a == b:
                continue
            pa, pb = positions[a], positions[b]
            dists.append((np.hypot(pa[0]-pb[0], pa[1]-pb[1]), b))
        dists.sort()
        for _, b in dists[:2]:
            pair = tuple(sorted((a, b)))
            if pair in used:
                continue
            used.add(pair)
            start = to_grid(*positions[a])
            goal  = to_grid(*positions[b])
            path = astar(start, goal)
            if len(path) < 4:
                path = straight_fallback(start, goal)
            if len(path) > 2:
                edges.append(NavEdge(a=a, b=b, path=path, kind="road"))
                for (py, px) in path:
                    if 0 <= py < h and 0 <= px < w:
                        road_mask[py, px] = 1.0

    road_mask = maximum_filter(road_mask, size=3)
    road_mask = gaussian_filter(road_mask, sigma=0.95)
    road_mask = np.clip(road_mask, 0.0, 1.0)

    # gently flatten / lower roads
    height = height * (1.0 - road_mask * 0.17) + 0.21 * road_mask
    height = np.clip(height, 0.0, 1.0)

    return nodes, edges, road_mask, height


# ============================================================
# 6. Slope & cliffs
# ============================================================

def slope_and_cliffs(height: np.ndarray, threshold: float = 0.078) -> Tuple[np.ndarray, np.ndarray]:
    gy, gx = np.gradient(height)
    slope = np.sqrt(gx ** 2 + gy ** 2)
    cliff = (slope > threshold).astype(np.float64)
    cliff = maximum_filter(cliff, size=2)
    return slope, cliff


# ============================================================
# 7. Vertex colors
# ============================================================

def compute_vertex_colors(
    height: np.ndarray,
    slope: np.ndarray,
    wet: np.ndarray,          # river + road influence
    mask: np.ndarray
) -> np.ndarray:
    h = height
    s = np.clip(slope * 9.0, 0.0, 1.0)

    sand  = np.array([0.78, 0.70, 0.48])
    grass = np.array([0.32, 0.52, 0.20])
    rock  = np.array([0.48, 0.45, 0.42])
    snow  = np.array([0.90, 0.92, 0.95])
    water = np.array([0.12, 0.32, 0.52])
    dirt  = np.array([0.45, 0.32, 0.18])

    color = np.zeros((*height.shape, 3), dtype=np.float64)

    # deep / shallow water
    water_m = h < 0.29
    color[water_m] = water

    # beach
    beach = (h >= 0.29) & (h < 0.37)
    color[beach] = sand

    # land
    land = h >= 0.37
    t = np.clip((h - 0.37) / 0.48, 0.0, 1.0)[..., None]
    base = grass * (1.0 - t) + rock * t
    # steep → rockier
    base = base * (1.0 - s[..., None] * 0.70) + rock * (s[..., None] * 0.70)
    color[land] = base[land]

    # high peaks
    color[h > 0.80] = snow

    # wet / path influence
    color = color * (1.0 - wet[..., None] * 0.28) + dirt * (wet[..., None] * 0.18)

    # subtle variation
    rng = np.random.default_rng(12345)
    noise = (rng.random((*height.shape, 1)) - 0.5) * 0.045
    color = np.clip(color + noise, 0.0, 1.0)
    return color


# ============================================================
# 8. Adaptive mesh (selective subdivision)
# ============================================================

def adaptive_mesh_from_height(
    height: np.ndarray,
    colors: np.ndarray,
    road_mask: np.ndarray,
    cliff_mask: np.ndarray,
    landmark_mask: np.ndarray,
    scale: float = 26.0,
    height_scale: float = 9.5,
) -> Tuple[list, list, list]:
    h, w = height.shape

    gy, gx = np.gradient(height)
    slope = np.sqrt(gx**2 + gy**2)

    importance = (
        (slope > 0.065).astype(float) * 1.6 +
        road_mask * 1.3 +
        cliff_mask * 1.9 +
        landmark_mask * 1.1
    )
    importance = gaussian_filter(importance, sigma=1.1)

    vertices = []
    vcolors = []
    faces = []
    vert_index = {}

    def add_vert(y: int, x: int) -> int:
        key = (y, x)
        if key in vert_index:
            return vert_index[key]
        px = (x / (w - 1) - 0.5) * scale
        pz = (y / (h - 1) - 0.5) * scale
        py = float(height[y, x]) * height_scale
        idx = len(vertices)
        vertices.append((px, py, pz))
        vcolors.append(tuple(float(c) for c in colors[y, x]))
        vert_index[key] = idx
        return idx

    step = 1
    for y in range(0, h - 1, step):
        for x in range(0, w - 1, step):
            if y + step >= h or x + step >= w:
                continue

            # decide density for this cell
            cell_imp = importance[y, x]
            if cell_imp > 0.38:
                # dense – still use the four corners (true mid-edge subdivision can be added)
                i00 = add_vert(y, x)
                i01 = add_vert(y, x + step)
                i10 = add_vert(y + step, x)
                i11 = add_vert(y + step, x + step)
                faces.append((i00, i01, i11))
                faces.append((i00, i11, i10))
            else:
                # same topology but we could skip some later; keep consistent for now
                i00 = add_vert(y, x)
                i01 = add_vert(y, x + step)
                i10 = add_vert(y + step, x)
                i11 = add_vert(y + step, x + step)
                faces.append((i00, i01, i11))
                faces.append((i00, i11, i10))

    return vertices, vcolors, faces


# ============================================================
# 9. Cliff wall generation
# ============================================================

def generate_cliff_walls(
    height: np.ndarray,
    cliff_mask: np.ndarray,
    scale: float = 26.0,
    height_scale: float = 9.5,
    wall_extra: float = 0.55,
) -> Tuple[list, list]:
    h, w = height.shape
    vertices = []
    faces = []

    def world(y, x, extra=0.0):
        px = (x / (w - 1) - 0.5) * scale
        pz = (y / (h - 1) - 0.5) * scale
        py = float(height[y, x]) * height_scale + extra
        return (px, py, pz)

    # Horizontal runs
    for y in range(h - 1):
        for x in range(w - 1):
            if cliff_mask[y, x] < 0.5:
                continue
            dh = abs(height[y, x] - height[y + 1, x])
            if dh < 0.045:
                continue
            base = len(vertices)
            extra = wall_extra
            vertices.append(world(y,     x,     0.0))
            vertices.append(world(y,     x + 1, 0.0))
            vertices.append(world(y,     x + 1, extra))
            vertices.append(world(y,     x,     extra))
            faces.append((base + 0, base + 1, base + 2))
            faces.append((base + 0, base + 2, base + 3))

    # Vertical runs
    for y in range(h - 1):
        for x in range(w - 1):
            if cliff_mask[y, x] < 0.5:
                continue
            dh = abs(height[y, x] - height[y, x + 1])
            if dh < 0.045:
                continue
            base = len(vertices)
            extra = wall_extra
            vertices.append(world(y,     x, 0.0))
            vertices.append(world(y + 1, x, 0.0))
            vertices.append(world(y + 1, x, extra))
            vertices.append(world(y,     x, extra))
            faces.append((base + 0, base + 1, base + 2))
            faces.append((base + 0, base + 2, base + 3))

    return vertices, faces


# ============================================================
# 10. OBJ writer with groups
# ============================================================

def write_grouped_obj(filename: str, meshes: Dict[str, Tuple[list, list]]):
    with open(filename, "w") as f:
        f.write("# N64-style procedural island\n")
        f.write("# Generated by island_n64_generator.py\n\n")
        offset = 0
        for group_name, (verts, faces) in meshes.items():
            if not verts:
                continue
            f.write(f"g {group_name}\n")
            for v in verts:
                f.write(f"v {v[0]:.5f} {v[1]:.5f} {v[2]:.5f}\n")
            for face in faces:
                f.write(f"f {face[0]+1+offset} {face[1]+1+offset} {face[2]+1+offset}\n")
            offset += len(verts)
            f.write("\n")
    print(f"Wrote {filename}")


# ============================================================
# Main generation
# ============================================================

def generate_island(seed: int = 42, resolution: int = 73,
                    out_path: str = "island_n64.obj") -> dict:
    shape = (resolution, resolution)
    print(f"Generating island  seed={seed}  resolution={resolution}")

    # 1. Landmarks
    landmarks = place_landmarks(seed, shape)
    print(f"  Landmarks: {[lm.name for lm in landmarks]}")

    # 2. Silhouette
    mask = generate_silhouette_mask(shape, seed)

    # 3. Base height + landmarks
    rng = seeded_rng(seed)
    height = fbm(shape, rng, octaves=6, persistence=0.47) * 0.58 + mask * 0.42
    height = apply_landmarks(height, landmarks, shape)
    height = normalize(height) * mask
    height = np.clip(height, 0.0, 1.0)

    # 4. Rivers
    flow = compute_flow_accumulation(height)
    height, river = carve_from_flow(height, flow)

    # 5. Roads / navigation
    nodes, edges, road_mask, height = build_navigation_graph(
        landmarks, height, mask, seed
    )
    print(f"  Nav edges: {len(edges)}")

    # 6. Cliffs
    slope, cliff_mask = slope_and_cliffs(height)

    # Combined wetness for coloring
    wet = np.clip(river + road_mask * 0.7, 0.0, 1.0)
    colors = compute_vertex_colors(height, slope, wet, mask)

    # Landmark density mask
    landmark_mask = np.zeros_like(height)
    yy, xx = np.ogrid[:shape[0], :shape[1]]
    for lm in landmarks:
        cy = lm.pos[1] * (shape[0] - 1)
        cx = lm.pos[0] * (shape[1] - 1)
        dist = np.sqrt((yy - cy)**2 + (xx - cx)**2)
        landmark_mask += np.clip(1.0 - dist / (lm.radius * max(shape) * 0.65 + 1e-6), 0, 1)
    landmark_mask = np.clip(landmark_mask, 0, 1)

    # 7. Adaptive terrain mesh
    terr_verts, terr_cols, terr_faces = adaptive_mesh_from_height(
        height, colors, road_mask, cliff_mask, landmark_mask
    )

    # 8. Cliff walls
    cliff_verts, cliff_faces = generate_cliff_walls(height, cliff_mask)

    print(f"  Terrain: {len(terr_verts)} verts, {len(terr_faces)} tris")
    print(f"  Cliffs : {len(cliff_verts)} verts, {len(cliff_faces)} tris")
    total_tris = len(terr_faces) + len(cliff_faces)
    print(f"  Total triangles: {total_tris}")

    # 9. Export
    meshes = {
        "terrain": (terr_verts, terr_faces),
        "cliffs":  (cliff_verts, cliff_faces),
    }
    write_grouped_obj(out_path, meshes)

    return {
        "seed": seed,
        "landmarks": landmarks,
        "nodes": nodes,
        "edges": edges,
        "height": height,
        "mask": mask,
        "river": river,
        "road_mask": road_mask,
        "cliff_mask": cliff_mask,
        "colors": colors,
        "total_tris": total_tris,
        "obj_path": out_path,
    }


if __name__ == "__main__":
    result = generate_island(seed=42, resolution=73, out_path="/home/workdir/artifacts/island_n64.obj")
    print("\nDone.")
    print(f"OBJ written to: {result['obj_path']}")
    print(f"Triangle count: {result['total_tris']}")
