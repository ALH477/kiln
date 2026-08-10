# N64-Ready Low-Poly Drone Enemy

**Game-ready 3D model** of a hostile drone enemy designed for Nintendo 64-style / retro low-poly aesthetics.

## Specs
- **Triangles**: 288 (very efficient, multiple can be on screen)
- **Vertices**: 192
- **Format**: Wavefront OBJ + MTL
- **Coordinate system**: Y-up, model faces -Z (standard for most engines)
- **Origin**: Centered
- **No textures required** — uses flat materials that map well to vertex colors or tiny 16-32px N64 textures

## Materials
| Material | Color Intent          | Use                  |
|----------|-----------------------|----------------------|
| Body     | Dark gunmetal gray    | Main chassis, arms   |
| Accent   | Enemy red             | Weapon pods, eye housing |
| Prop     | Near-black            | Rotor blades         |
| Eye      | Bright glowing red    | Sensor / targeting eye |

## Files
- `n64_drone_enemy.obj` — the mesh
- `n64_drone_enemy.mtl` — materials

## How to use
1. Import OBJ into Godot, Unity, Blender, or your engine.
2. For pure N64 authenticity: convert materials to vertex colors or bake to a single 32x32 or 64x64 palette texture.
3. Scale to fit your world (default is roughly 1.5 units wide).
4. Animate: rotate the 4 prop groups, bob the body slightly for hover, pulse the Eye material emissive.

## Design notes
- Angular, chunky silhouette readable at N64 resolutions and distances.
- Clear front (red eye) so players can tell facing direction.
- Side weapon pods for attack variants.
- Simple cross-blade props (look good spinning even at low poly).
- Landing skids for grounded poses.

Created for video game use. Free to modify, use in commercial or free projects.
