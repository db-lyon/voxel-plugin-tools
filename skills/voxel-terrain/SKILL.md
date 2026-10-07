---
name: voxel-terrain
description: Build procedural Voxel Plugin 2 terrain through ue-mcp's voxel category - height graph with exposed parameters, voxel world, graph stamp with per-stamp overrides, surface types on a mega material, sculpt layers, and voxel PCG sampling. Use when creating or editing voxel terrain, voxel graphs, or voxel stamps.
---

# Voxel terrain with ue-mcp

Every step below is a `voxel(action=...)` call. All of them run as native handlers; never fall back to Python for Voxel work.

Each action checks its parameters against its C++ contract before it runs (`tools(action="describe", category="voxel", method=...)` lists them). Wrong types, unknown keys, fractional integers, out-of-range numbers and enum values in another spelling are refused, not coerced, and an op or stamp kind refuses fields it does not read. Read the refusal and fix the call; it names the field.

## 0. Preflight: shader hooks

Run `voxel_shader_hooks_status` first. Voxel materials render only when Voxel's patches to the engine shaders are applied; without them every generated surface shader compiles its voxel code out, so terrain shows the gray grid checker (Nanite) or renders near-black (non-Nanite), and nothing logs an error. If `allActive` is false: close the editor, run `UnrealEditor-Cmd.exe <project>.uproject -run=ApplyVoxelShaderHooks`, restart (shaders recompile). The patch lives in the engine install, so every machine that compiles shaders needs it.

A new mega material's `NonNaniteMaterialType` defaults to `Custom` with no material: set it to `Generated` (`voxel_mega_material_set_surfaces`, `nonNaniteMaterialType: "Generated"`) or non-Nanite rendering falls back to the grid.

## 1. Assets

Create through `voxel_asset_create` so graph types come from their factory (a new height graph already has Advanced Noise 2D wired to Output Height):

- `height_graph` for the terrain shape
- `surface_type` per ground material, then `voxel_surface_type_set` with `material`
- `mega_material`, then `voxel_mega_material_set_surfaces` with the surface types

Existing assets are returned, never overwritten (`existed: true`).

A variant of a graph that differs only in parameter values is an instance, not a copy: `voxel_asset_create` with `instanceOf` set to the base graph, then `voxel_graph_set_parameter_default` on the instance. Instances refuse node edits; change the base and every instance follows.

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

`voxel_actor_spawn` `kind: "height_sculpt"` then `voxel_height_sculpt` (`sculpt_height`, `flatten`, `smooth`, `paint_surface`). A `brush` names its `type` (`Circular`, `Alpha` or `Pattern`, the last two with a `texture`). Bind a save asset with `voxel_sculpt_asset_set` so the edits live outside the level.

## 6. PCG on the voxel surface

Create the engine graph with `pcg(action="create_graph")`, add `wait_for_world` and `sampler_v2` with `voxel_pcg_add_node`, set spacing and `resolveSmartSurfaceTypes` with `voxel_pcg_configure_sampler`. The sampler writes one attribute per surface type; filter spawners on it.

Feed the sampler's `Bounding Shape` pin from `wait_for_world`. The per-surface attribute is named after the surface type asset (`ST_Grass`); in `pcg(set_node_settings)` write the selector as `PCGBegin(ST_Grass)PCGEnd`.

One sampler call refuses more than 1,048,576 candidate positions ((bounds width / distanceBetweenPoints)^2) and returns empty data with no log line. Large areas need a partitioned PCG component (`bIsComponentPartitioned`, `GenerationTrigger: GenerateAtRuntime`; works without World Partition in 5.8): each partition cell samples its own bounds, so keep the partition grid under 1024 x distanceBetweenPoints. Never split sampling with a `Loop` over cells: Voxel's PCG tracker keys dependencies by component and node, so every iteration overwrites the last and only one cell reacts to terrain edits. In game worlds the tracker only refreshes runtime-generated components, so anything that must follow voxel edits in-game has to be `GenerateAtRuntime`. For editor preview set `bTreatEditorViewportAsGenerationSource` on the level's PCG world actor.

Points within 1 m of a stamp's edge get a valid height and a NaN normal (the scatter's 100 cm gradient step samples past the edge), so their rotation is NaN and the ISM they spawn into gets NaN bounds and is culled whole. Filter `$Rotation.W > -2` right after the sampler and keep sampling bounds inside the terrain.

Set the PCG component's editing mode to Preview so generated output regenerates on load instead of being saved into the level.

A runtime-generated component's partition cells belong to PCG's runtime scheduler, and every stamp change under them makes Voxel's tracker refresh them. To clear such a component, set `bActivated` false and call `UPCGSubsystem::RefreshRuntimeGenExecutionSource(Component, EPCGChangeType::GenerationGrid)`; never `CleanupLocal` on it. A direct cleanup races the scheduler's own and trips PCG's `bAreResourcesInaccessible` ensures. `UPCGComponent::Cleanup` refuses these components for the same reason.

## 7. Terrain edits from PCG

Put a second height layer above the base one in the world's layer stack. Systems that place things sample the base layer; stamps they spawn (`stamp_spawner` with a height graph or heightmap template) go to the edits layer; scatter samples the edits layer. Placement then never invalidates itself, and scatter regenerates when edits change under it. The stamp spawner uses the full point transform including scale: reset scale when a footprint is already in a graph parameter. Map per-point values onto graph parameters with `SpawnedGraphParameterOverrideDescriptions` and onto stamp properties with `SpawnedStampPropertyOverrideDescriptions`; enum properties (BlendMode) cannot be set from a string attribute, so split points by value into spawners whose templates carry each mode.

A surface type parameter override (`FVoxelParameterValueOverride`) must hold the parameter's exposed type, the surface asset: build it with `FVoxelPinValue(Parameter.Type.GetExposedType())` and `ImportFromUnrelated(FVoxelPinValue::Make(Surface))`. An override in the inner `FVoxelSurfaceType` is orphaned by `FixupParameterOverrides` and the stamp silently carves with the graph's default surface. Voxel spline components carry spline metadata, so generic spline-point tools refuse them; edit them through the spline component API.
