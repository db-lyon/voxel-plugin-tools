---
name: voxel-pcg
description: "Use when building PCG on Voxel Plugin 2 terrain through ue-mcp: sampling voxel layers with the Voxel Sampler, scattering by surface type, partitioned and runtime-generated components, regeneration when stamps change, spawning voxel stamps from PCG points, and routing over voxel ground. Pulls in for any PCG graph that reads or writes voxel terrain."
---

# PCG on voxel terrain with ue-mcp

Voxel PCG nodes read voxel layers directly (a stack and a layer), not an `AVoxelWorld`. Voxel's PCG tracker records which stamps each node read and regenerates the component when those stamps change. Most failures here are silent: an empty point set, a component that never reacts, a level that gets dirtied. Read the rules before building, not after the first empty result.

The engine graph and its edges are core `pcg` actions; the Voxel nodes and their settings are `voxel` actions. Terrain and stamp setup is in the `voxel-plugin-tools-voxel-terrain` skill.

## Build the graph

1. `pcg(action="create_graph", name="PG_Scatter", packagePath="/Game/PCG")`.
2. `voxel(action="voxel_pcg_add_node", graphPath="/Game/PCG/PG_Scatter", nodeType="sampler_v2")`, and `nodeType="wait_for_world"`. Keep each returned `name`: it is the `nodeName` / `sourceNode` the `pcg` actions take.
3. `voxel(action="voxel_pcg_configure_sampler", graphPath=..., node=<name>, stack=..., layer=<height layer>, distanceBetweenPoints=400)`.
4. Wire with `pcg(action="connect_nodes")`; engine nodes come from `pcg(action="add_node")`. Every node but the samplers, Voxel or engine, is configured with `pcg(action="set_node_settings")`.
5. Regenerate with `pcg(action="execute")`, then `editor(action="search_log")` from that call onward for `LogPCG` errors and `Voxel: Error` lines. A run is not verified while any remain.

| `nodeType` | Node | What it does |
|---|---|---|
| `sampler_v2` | Voxel Sampler Experimental | Points on a layer: Grid, Sobol or Halton spacing on height layers, surface cells on volume layers |
| `sampler` | Voxel Sampler | Density-based random points per cell |
| `wait_for_world` | Wait For Voxel World | Passes its input through once every voxel world with a runtime has finished its pending state (a world with no runtime is not waited on) |
| `query` | Voxel Query | Writes height or distance, surface types and metadata onto existing points |
| `projection` | Voxel Projection | Projects points onto the surface, optionally aligning rotation |
| `stamp_spawner` | Voxel Stamp Spawner | One stamp per point into a managed instanced stamp component |
| `create_spline` | Create Voxel Spline | Spawns a stamp actor with a height or volume spline stamp from spline data |
| `call_graph` | Call Voxel Graph | Runs a Voxel PCG graph (`voxel_asset_create` `type: "pcg_graph"`) over points |
| `spawn_actor` | Spawn Actor with Voxel Graph | Spawns an actor per point and overrides its graph parameters from attributes |
| `apply_on_graph` | Apply on Voxel Graph | Sets graph parameters on the objects an attribute references |
| `elevation_isolines` | Voxel Elevation Isolines | Contour points or splines of a height layer |

## Sampler rules

- **Set `layer` explicitly.** A new sampler's layer is Voxel's built-in default *volume* layer (`/Voxel/Default`), so it takes the 3D path even over height terrain. Point it at the height layer the terrain stamps write to.
- **One attribute per surface type.** `sampler_v2` writes a float per registered surface type, named after the surface type asset (`ST_Grass`), holding its blend weight; types absent from the bounds still get the attribute, at 0. With `resolveSmartSurfaceTypes` (default true; `sampler` on a height layer always resolves) smart surface types resolve to concrete ones; otherwise points carry the smart type's own name. `metadatasToQuery` attributes are named after the metadata asset. An attribute selector imported as text, as `pcg(action="set_node_settings")` writes it, takes the selector struct's text form: `PCGBegin(ST_Grass)PCGEnd`.
- **Bounds.** The Bounding Shape pin is intersected with the component's actor bounds; with no shape the actor bounds are used, and `unbounded` without a shape samples everywhere.
- **The candidate cap is silent.** On a height layer one `sampler_v2` call refuses more than 1,048,576 candidate positions, counted in tiles of 16 x `distanceBetweenPoints`, and outputs empty data with no log line. At 100 cm spacing that is about 1 km square, less when the bounds do not line up with the tile grid. Volume layers have no cap.
- **Z bounds matter on height layers too.** `sampler_v2` logs `Invalid distance between points` when the bounds' Z extent holds no multiple of `distanceBetweenPoints`.
- **NaN rotations at layer edges.** Normals come from heights 50 cm either side of each point. A point within 50 cm of where the layer ends has a valid height and a NaN normal, so its rotation is NaN; an ISM given one NaN transform gets NaN bounds and is culled whole. `sampler_v2` keeps such points (`sampler` drops them): filter `$Rotation.W > -2` right after it and keep bounds inside the terrain.
- **Metadata on height layers.** `sampler_v2` drops its metadata attributes when any candidate was discarded (a NaN height, or outside the Z bounds), because it attaches the unfiltered buffers: one ensure callstack in the log, then the attributes are missing. Expect them only when every candidate became a point, or query metadata afterwards with a `query` node.
- **Order behind the world.** Feed `wait_for_world`'s output into the sampler's `Dependency` pin, or route the bounding shape through it, so sampling waits for the voxel world.

## Regeneration

The tracker keys what a node read by component and node settings. Every run of the same settings in one component replaces the previous run's record, so a `Loop` over cells, or one subgraph instanced twice, leaves only the last run tracked: only that cell reacts to terrain edits. Never split sampling with a Loop. Split it with a partitioned component.

- In an editor (non-game) world the tracker regenerates any component whose inputs changed. In game worlds, PIE included, it refreshes only components with `GenerationTrigger` `GenerateAtRuntime`; anything that must follow voxel edits in game has to be runtime-generated.
- Large areas: a partitioned component (`bIsComponentPartitioned`, `GenerationTrigger: GenerateAtRuntime`). Each partition cell samples its own bounds, so keep the grid (`PartitionGridSize` on the level's PCG world actor, or the graph's hierarchical grid sizes) under the sampler cap. For editor preview set `bTreatEditorViewportAsGenerationSource` on the PCG world actor.
- Component properties go through `level(action="set_component_property")`; PCG world actor properties (`PartitionGridSize`, `bTreatEditorViewportAsGenerationSource`) through `level(action="set_actor_property")`.
- To clear a runtime-generated component, set `bActivated` false and call `UPCGSubsystem::RefreshRuntimeGenExecutionSource(Component, EPCGChangeType::GenerationGrid)`, which performs the full cleanup of its partition cells. Never `CleanupLocal` it: its cells belong to PCG's runtime scheduler, a direct cleanup races the scheduler's own, and PCG's `bAreResourcesInaccessible` ensures fire.

## Persistence

`EPCGEditorDirtyMode::Preview` (the component's Editing Mode) neither dirties the level nor saves generated resources; runtime-generated components always behave as Preview. Create Voxel Spline is the exception: it spawns its stamp actors into the level with default spawn parameters in every editing mode, so even Preview generation dirties the level with them.

## Terrain edits from PCG

Use two height layers in the world's layer stack (`voxel_layer_stack_set`), base below and edits above. Systems that place things sample the base layer; the stamps they spawn go to the edits layer; scatter samples the edits layer. Placement then never invalidates itself, and scatter regenerates when edits change under it.

- The Stamp Spawner places each stamp with the full point transform, scale included. Reset point scale when the footprint already lives in a graph parameter.
- `SpawnedGraphParameterOverrideDescriptions` maps point attributes to graph parameters (height and volume graph templates only); `SpawnedStampPropertyOverrideDescriptions` maps them to stamp properties.
- An enum stamp property (`BlendMode`) cannot come from a string attribute: the override is skipped with `Cannot convert type`. Write the enum's integer value (`EVoxelHeightBlendMode`: Max 0, Min 1, Override 2) as an int attribute.

## Routing over voxel ground

PCG Pathfinding reaches a goal by 3D distance from the sampled cloud, so a start or goal floating above sloping voxel ground fails with `The search could not be completed`. Project every start and goal onto the layer the cloud was sampled from. `bAcceptPartialPath` (on by default) returns the path to the reachable point nearest the goal, tagged `PartialPath`; that tag shows which end is stuck.

## Before reporting done

- [ ] Every sampler names its stack and a layer of the right type.
- [ ] No sampler runs inside a Loop or a repeated subgraph.
- [ ] Bounds stay under the cap and inside the terrain; NaN rotations are filtered before any spawner.
- [ ] Anything that must react to voxel edits in game is `GenerateAtRuntime`.
- [ ] The log from the last `pcg(action="execute")` has no PCG or Voxel errors, and the output was looked at in a viewport capture.
