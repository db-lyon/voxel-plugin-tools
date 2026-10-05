---
name: voxel-terrain
description: Build procedural Voxel Plugin 2 terrain through ue-mcp's voxel category - height graph with exposed parameters, voxel world, graph stamp with per-stamp overrides, surface types on a mega material, sculpt layers, and voxel PCG sampling. Use when creating or editing voxel terrain, voxel graphs, or voxel stamps.
---

# Voxel terrain with ue-mcp

Every step below is a `voxel(action=...)` call. All of them run as native handlers; never fall back to Python for Voxel work.

## 0. Preflight: shader hooks

Run `voxel_shader_hooks_status` first. Voxel materials render only when Voxel's patches to the engine shaders are applied; without them every generated surface shader compiles its voxel code out, so terrain shows the gray grid checker (Nanite) or renders near-black (non-Nanite), and nothing logs an error. If `allActive` is false: close the editor, run `UnrealEditor-Cmd.exe <project>.uproject -run=ApplyVoxelShaderHooks`, restart (shaders recompile). The patch lives in the engine install, so every machine that compiles shaders needs it.

A new mega material's `NonNaniteMaterialType` defaults to `Custom` with no material: set it to `Generated` (`voxel_mega_material_set_surfaces`, `nonNaniteMaterialType: "Generated"`) or non-Nanite rendering falls back to the grid.

## 1. Assets

Create through `voxel_asset_create` so graph types come from their factory (a new height graph already has Advanced Noise 2D wired to Output Height):

- `height_graph` for the terrain shape
- `surface_type` per ground material, then `voxel_surface_type_set` with `material`
- `mega_material`, then `voxel_mega_material_set_surfaces` with the surface types

Existing assets are returned, never overwritten (`existed: true`).

## 2. Shape the graph

1. `voxel_graph_read` to see nodes and pins.
2. `voxel_graph_add_parameter` for each knob (`float`, `seed`, ...), with a default.
3. `voxel_graph_add_node` with `nodeType: "Parameters|<name>"` for its getter, then `voxel_graph_connect` it to the pin it drives.
4. `voxel_graph_list_node_types` with query words finds other nodes (masks: `Box Falloff 2D`, `Sphere Falloff 2D`; noise: `Perlin Noise 2D`, `Advanced Noise 2D`; math: `Lerp`, `Smooth Step`).
5. Wire surfaces into Output Height's `SurfaceType` pin (`Make Surface Type Blend`, `Blend Surface Types`).

`voxel_graph_export_t3d` and `voxel_graph_import_t3d` copy node groups between graphs.

## 3. World and stamp

1. `voxel_world_spawn` with `megaMaterial` (and `layerStack` if not the default).
2. `voxel_actor_spawn` `kind: "stamp"`; keep the returned `actorPath`.
3. `voxel_stamp_set` `kind: "height_graph"`, `asset`, `blendMode`, `parameters: {name: value}`.
4. `voxel_stamp_set_parameters` to retune without touching the rest; `voxel_stamp_read` to confirm.
5. More stamps with higher `priority` and `blendMode: "Override"` flatten or shape regions on top (a graph built from `Box Falloff 2D`).

## 4. Verify

- `voxel_world_status` until `isReady`.
- `voxel_query_layer` with sample `points` (and `querySurface: true`) to check heights and which surface won.
- Capture the viewport; a call succeeding is not proof the terrain looks right.

## 5. Sculpt layer

`voxel_actor_spawn` `kind: "height_sculpt"` then `voxel_height_sculpt` (`sculpt_height`, `flatten`, `smooth`, `paint_surface`). Bind a save asset with `voxel_sculpt_asset_set` so the edits live outside the level.

## 6. PCG on the voxel surface

Create the engine graph with `pcg(action="create_graph")`, add `wait_for_world` and `sampler_v2` with `voxel_pcg_add_node`, set spacing and `resolveSmartSurfaceTypes` with `voxel_pcg_configure_sampler`. The sampler writes one attribute per surface type; filter spawners on it.
