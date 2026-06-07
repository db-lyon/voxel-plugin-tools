# voxel-plugin-tools

[Voxel Plugin](https://voxelplugin.com) actions for [ue-mcp](https://github.com/db-lyon/ue-mcp). 60 actions and 1 flow.

## Install

```bash
ue-mcp plugin install voxel-plugin-tools
```

Adds an entry under `plugins:` in `ue-mcp.yml`. Restart ue-mcp.

## Requirements

- ue-mcp `>= 1.0.15`
- Voxel Plugin enabled in your `.uproject` (`Plugins[].Name == "Voxel"`)
- Unreal Engine 5.7
- Node `>= 18` to build (20+ recommended)

## Usage

Actions are called as `<category>(action="voxel_<name>", ...)`. Three categories: `level`, `asset`, `pcg`.

Spawn a rendering world:

```text
flow(action="run", flowName="voxel_hello_world", params={ label: "MyVoxelWorld" })
```

Check readiness before sculpting, stamping, or sampling:

```text
level(action="voxel_is_world_ready", actorLabel="MyVoxelWorld")
# => { returnValues: { ReturnValue: "true" } }
```

Build terrain from a heightmap:

```text
asset(action="voxel_create_asset", name="Terrain01", type="heightmap")
level(action="voxel_spawn_stamp_actor", label="Terrain")
level(action="voxel_set_heightmap_stamp", actorLabel="Terrain",
      source="/Game/Terrain01.Terrain01", blendMode="max")
```

## Actions

[`ue-mcp.plugin.yml`](ue-mcp.plugin.yml) is the source of truth for parameters and the C++ header each action wraps. Reference docs are under [`docs/`](docs/). Each action wraps exactly one host call; sequencing belongs in flows.

### level: spawns

| Action | Wraps / does |
|---|---|
| `voxel_spawn_world` | Place an `AVoxelWorld` in the active level |
| `voxel_spawn_stamp_actor` | Place an `AVoxelStampActor` (owns one stamp) |
| `voxel_spawn_height_sculpt_actor` | Place an `AVoxelHeightSculptActor` (2D/height sculpting target) |
| `voxel_spawn_volume_sculpt_actor` | Place an `AVoxelVolumeSculptActor` (3D/volume sculpting target) |
| `voxel_spawn_collision_baker` | Place an `AVoxelCollisionBaker` (bakes collision for a layer in a radius) |
| `voxel_spawn_debug_actor` | Place an `AVoxelDebugActor` (visualizes a layer; dev tool) |

### level: world lifecycle and status

| Action | Wraps / does |
|---|---|
| `voxel_is_world_ready` | Read `IsVoxelWorldReady()`; main gate before sculpt/stamp/sample |
| `voxel_get_world_progress` | Read `GetProgress()` (0..1; 1 when idle) |
| `voxel_is_runtime_created` | Read `IsRuntimeCreated()` (initialized, not necessarily rendered) |
| `voxel_is_processing_new_state` | Read `IsProcessingNewState()` (regeneration in flight) |
| `voxel_get_num_pending_tasks` | Read `GetNumPendingTasks()` (queued background work) |
| `voxel_create_runtime` | Call `CreateRuntime()` (initialize the voxel runtime) |
| `voxel_destroy_runtime` | Call `DestroyRuntime()` (free meshes/collision/nav) |

### level: world config

| Action | Wraps / does |
|---|---|
| `voxel_set_world_layer_stack` | Set `LayerStack` (the `UVoxelLayerStack` the world generates/renders) |
| `voxel_set_world_mega_material` | Set `MegaMaterial` (surface shading) |
| `voxel_set_world_voxel_size` | Set `VoxelSize` in cm (default 100; lower is finer and heavier) |
| `voxel_set_world_enable_nanite` | Set `bEnableNanite` (default true) |
| `voxel_set_world_enable_navigation` | Set `bEnableNavigation` (default true; needed for AI nav) |
| `voxel_set_world_enable_lumen` | Set `bEnableLumen` (default false; expensive) |
| `voxel_set_world_lod_quality` | Set `LODQuality` (detail vs performance beyond VoxelSize) |

### level: volume sculpting (`AVoxelVolumeSculptActor`)

| Action | Wraps / does |
|---|---|
| `voxel_volume_sculpt_sphere` | Add/remove a sphere |
| `voxel_volume_sculpt_cube` | Add/remove a rounded, rotated cube |
| `voxel_volume_flatten` | Flatten the surface along a normal |
| `voxel_volume_smooth` | Smooth/blur a region |
| `voxel_volume_sculpt_surface` | Progressively sculpt the surface |
| `voxel_volume_sculpt_angle` | Move the surface toward a plane |
| `voxel_volume_paint_surface` | Paint a surface type on a sculpt stamp |
| `voxel_volume_apply_sculpt_graph` | Apply a volume sculpt graph at a location |
| `voxel_volume_clear_sculpt_data` | Clear all sculpt data (reset edits) |
| `voxel_volume_clear_sculpt_cache` | Free the sculpt cache (keeps data) |

### level: height sculpting (`AVoxelHeightSculptActor`)

| Action | Wraps / does |
|---|---|
| `voxel_height_sculpt_height` | Raise/lower height |
| `voxel_height_flatten` | Flatten toward a target height |
| `voxel_height_smooth` | Smooth/blur a height region |
| `voxel_height_paint_surface` | Paint a surface type on a height sculpt stamp |
| `voxel_height_apply_sculpt_graph` | Apply a height sculpt graph at a location |
| `voxel_height_clear_sculpt_data` | Clear all sculpt data (reset edits) |
| `voxel_height_clear_sculpt_cache` | Free the sculpt cache (keeps data) |

### level: stamps

| Action | Wraps / does |
|---|---|
| `voxel_add_stamp_component` | Attach a `UVoxelStampComponent` to an actor |
| `voxel_set_heightmap_stamp` | Build a heightmap stamp (`UVoxelHeightmap`) and apply it |
| `voxel_set_height_graph_stamp` | Build a height-graph stamp (`UVoxelHeightGraph`) and apply it |
| `voxel_set_volume_graph_stamp` | Build a volume-graph stamp (`UVoxelVolumeGraph`) and apply it |
| `voxel_set_mesh_stamp` | Build a mesh stamp (`UVoxelStaticMesh`, tricubic) and apply it |
| `voxel_set_height_spline_stamp` | Build a height-spline stamp (`UVoxelHeightSplineGraph`) and apply it |
| `voxel_set_volume_spline_stamp` | Build a volume-spline stamp (`UVoxelVolumeSplineGraph`) and apply it |
| `voxel_update_stamp_actor` | Call `UpdateStamp()` to re-apply after property changes |

### level: sculpt save assets

| Action | Wraps / does |
|---|---|
| `voxel_set_sculpt_save_asset` | Link a sculpt actor to an external save asset (type must match) |
| `voxel_get_sculpt_save_asset` | Read the linked save asset path (`VOXEL_SAVE_ASSET=` line) |

### level: query and export

| Action | Wraps / does |
|---|---|
| `voxel_query_voxel_layer` | Sample one point; returns `FVoxelQueryResult` (`VOXEL_QUERY_RESULT=` line) |
| `voxel_multi_query_voxel_layer` | Batch-sample many points in one call |
| `voxel_export_voxel_data_to_render_target` | Write a height layer to a `UTextureRenderTarget2D` (`VOXEL_EXPORT_OK=` line) |

### level: instanced stamps (`UVoxelInstancedStampComponent`)

| Action | Wraps / does |
|---|---|
| `voxel_add_instanced_stamp_component` | Attach the component (many stamps in one component) |
| `voxel_clear_instanced_stamps` | Remove every stamp and refresh |
| `voxel_update_instanced_stamps` | Refresh all stamps (apply pending edits) |
| `voxel_count_instanced_stamps` | Report stamp count (`VOXEL_STAMP_COUNT=` line) |
| `voxel_remove_instanced_stamp` | Remove one stamp by index and refresh |

### level: no-clipping (`UVoxelNoClippingComponent`)

| Action | Wraps / does |
|---|---|
| `voxel_add_no_clipping_component` | Attach the component (keeps owner from clipping through terrain) |
| `voxel_set_no_clipping_layer` | Configure the solid-ground layer and auto-adjust |

### asset

| Action | Wraps / does |
|---|---|
| `voxel_create_asset` | Create a Voxel asset by type (`VOXEL_CREATED_ASSET=` line) |
| `voxel_set_asset_property` | Set one editor property on a Voxel asset and save it |

`voxel_create_asset` types: `surface_type`, `smart_surface_type`, `layer_stack`, `mega_material`, `heightmap`, `static_mesh`, `height_graph`, `volume_graph`, `scatter_graph`, `height_spline_graph`, `volume_spline_graph`, `height_sculpt_graph`, `volume_sculpt_graph`, `surface_type_graph`, `height_sculpt_save`, `volume_sculpt_save`, `pcg_graph`.

### pcg

| Action | Wraps / does |
|---|---|
| `voxel_pcg_add_node` | Add a VoxelPCG node to a `UPCGGraph`; configure after with `pcg.set_node_settings` |

`voxel_pcg_add_node` node types: `call_graph`, `sampler`, `sampler_v2`, `query`, `stamp_spawner`, `spawn_actor`, `apply_on_graph`, `create_spline`, `projection`, `elevation_isolines`, `wait_for_world`, `layer_sampler`, `landscape_sampler`, `landscape_projection`, `wait_for_landscape`.

## Flows

| Flow | Sequence |
|---|---|
| `voxel_hello_world` | `voxel.spawn_world`, then `level.set_actor_property LayerStack` (bundled default) |

## Parameters

- `location`, `rotation`, `scale` are `{ x, y, z }` objects.
- Asset and object references are content paths (`/Game/Foo.Foo`) or class paths (`/Script/...`).
- Height stamp blend modes: `max | min | override`. Volume stamp blend modes: `additive | subtractive | intersect | override`.
- Return values come back as strings at `data.returnValues.ReturnValue`.
- Python-backed actions (stamps, asset create/set) report results on marker lines (`VOXEL_CREATED_ASSET=`, `VOXEL_QUERY_RESULT=`, and so on).
- `voxel_set_asset_property` takes `value` as a string; numeric and boolean strings are coerced. For a UObject reference, set `valueIsAsset: true`.
- `level` reads accept `world` (`editor` or `pie`).

## Architecture

Task classes compile to `dist/` and load into ue-mcp, which bridges to the editor:

```
MCP client -> ue-mcp server -> WebSocket bridge (C++ plugin) -> UE editor
```

## Develop

```bash
git clone https://github.com/db-lyon/voxel-plugin-tools.git
cd voxel-plugin-tools
npm install
npm run build      # clean + tsc
npm run check      # static-check + schema-drift-check
```

See [`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

MIT. See [`LICENSE`](LICENSE).
