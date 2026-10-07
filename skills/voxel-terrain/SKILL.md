---
name: voxel-terrain
description: "Use when building or editing Voxel Plugin 2 terrain through ue-mcp's voxel category: the shader-hook preflight, layer stacks, the voxel world, height and volume stamps (graph, heightmap, mesh, spline, instanced) with per-stamp parameter overrides, caves and overhangs, surface types on a mega material, sculpt actors and their save assets, and verifying terrain with layer queries. Pulls in for any voxel world, stamp, terrain shape, cave or terrain material task."
---

# Voxel terrain with ue-mcp

Every step is a `voxel(action=...)` call, and every one runs as a native handler: never fall back to Python for Voxel work. Each action checks its call against a C++ contract before it runs (`tools(action="describe", category="voxel", method=...)` shows it). Wrong types, unknown keys, fractional integers, out-of-range numbers and enum values in another spelling are refused, and an op or stamp kind refuses fields it does not read. Read the refusal and fix the call; it names the field.

Graph authoring is the `voxel-plugin-tools-voxel-graphs` skill; PCG on voxel terrain is `voxel-plugin-tools-voxel-pcg`.

## How the pieces fit

- A **layer stack** lists height layers and volume layers, bottom to top. Height layers build on the one below; the first volume layer starts from the top height layer, turned into a distance field `maxDistance` up and down; volume layers build on the one below.
- A **stamp** writes into one layer. Within a layer, stamps apply in `priority` order (higher applies later) and combine by `blendMode`: height `Max`, `Min`, `Override`; volume `Additive` (union), `Subtractive` (carve), `Intersect`, `Override`.
- An **`AVoxelWorld`** renders its stack's top layer (the last volume layer, or the last height layer when there is none) with its **mega material**. Stamps exist without a world: `voxel_query_layer` and the PCG samplers read layers directly.
- **Sculpt actors** are stamps holding sculpt data; they sculpt into a layer of a world's stack.

## 0. Preflight: shader hooks

`voxel(action="voxel_shader_hooks_status")` before any material work. Voxel materials render only when Voxel's patches to the engine's shaders (`MaterialTemplate.ush` and the Nanite shaders) are applied; without them every generated mega material compiles its voxel code out and all material attributes read zero, so terrain shows a gray grid checker (Nanite) or renders near-black (non-Nanite), and nothing reaches the log (the editor shows only a transient notification). If `allActive` is false, follow the `fix` in the result: close the editor, run `UnrealEditor-Cmd.exe <project>.uproject -run=ApplyVoxelShaderHooks`, restart so shaders recompile. The patch lives in the engine install, so every machine that compiles shaders needs it, and anything that rewrites engine files removes it.

## 1. Assets

Create through `voxel(action="voxel_asset_create")` so each asset comes from Voxel's own factory. Existing assets are returned, never overwritten (`existed: true`).

- `height_graph` (or `volume_graph`) for the shape; the voxel-graphs skill covers editing it. A new height graph is Voxel's template: Advanced Noise 2D into Output Height, bounds from Make Box 2D From Radius, no parameters. A new volume graph is a sphere of radius 1000 cm.
- `height_layer`, `volume_layer` and `layer_stack` when the default stack is not enough, then `voxel(action="voxel_layer_stack_set", assetPath=..., heightLayers=[...], volumeLayers=[...])`, bottom to top.
- `surface_type` per ground material, then `voxel(action="voxel_surface_type_set", assetPath=..., material=...)`.
- `mega_material`, then `voxel(action="voxel_mega_material_set_surfaces", assetPath=..., surfaceTypes=[...], nonNaniteMaterialType="Generated")`. A new mega material's non-Nanite type is `Custom` with no material, so non-Nanite rendering falls back to Voxel's grid material until it is `Generated` or given `customNonNaniteMaterial`.

A variant of a graph that differs only in parameter values is an instance (`instanceOf`), not a copy.

## 2. World

`voxel(action="voxel_world_spawn", label="World", megaMaterial=..., layerStack=...)` takes the stack, mega material and voxel size; everything else goes through `voxel(action="voxel_world_configure")` with details-panel semantics (`enableNanite`, `lodQuality`, collision and navigation settings). `voxel(action="voxel_world_status")` reports `isReady`, `progress` and `pendingTasks`; `voxel(action="voxel_world_runtime", op="destroy")` then `op="create"` rebuilds the runtime. Spawns are refused during PIE.

## 3. Stamps

1. `voxel(action="voxel_actor_spawn", kind="stamp", label="Terrain", location=...)`; key every later call on the returned `actorPath`, since stamp actors relabel themselves from their stamp.
2. `voxel(action="voxel_stamp_set", actorPath=..., kind="height_graph", asset=..., layer=..., blendMode="Max", priority=0, parameters={Amplitude: 3000})`, where `parameters` names parameters the graph declares. The component's transform places the stamp. Setting the same kind again keeps every field you leave out; another kind replaces the stamp (`replacedKind`); pointing a graph kind at another graph clears its overrides (`overridesReset`).
3. `voxel(action="voxel_stamp_set_parameters", actorPath=..., values={...})` retunes graph parameters and keeps everything else; `voxel(action="voxel_stamp_read")` confirms what the component stored.

| `kind` | Asset | Layer |
|---|---|---|
| `height_graph` | UVoxelHeightGraph | height |
| `volume_graph` | UVoxelVolumeGraph | volume |
| `heightmap` | UVoxelHeightmap (`surfaceType` sets its default surface) | height |
| `mesh` | UVoxelStaticMesh (`surfaceType`, `useTricubic`) | volume |
| `height_spline` / `volume_spline` | spline graph, on a stamp actor only | height / volume |

Fields: `smoothness` is the blend width in centimetres (default 100); `behavior` limits what the stamp writes (shape, surface type, metadata); `applyOnVoid: false` applies only where an earlier stamp already applied, which keeps a stamp inside terrain that earlier stamps already built (ignored by `Override` and `Intersect`). A stamp with a higher `priority` and `blendMode: "Override"` replaces, within its own bounds, what lower-priority stamps produced: the way to flatten or reshape a region on top of the terrain.

Many copies of one stamp go on one actor: `voxel(action="voxel_component_add", kind="instanced_stamp")`, then `voxel(action="voxel_instanced_stamps", op="add", kind=..., asset=..., transforms=[...])`. Transforms are world space unless `relativeToComponent`; `remove` empties a slot without shifting indices.

Spline stamps take their curve from the stamp actor's `UVoxelSplineComponent`, which the stamp component creates once `voxel_stamp_set` gives it a spline kind. `level(action="set_spline_points")` refuses it, since it carries Voxel's per-point metadata; set it with `voxel(action="voxel_spline_set_points", actorPath=..., points=[{"location": {"x": 0, "y": 0, "z": 0}}, {"location": {"x": 2000, "y": 0, "z": 0}}], metadata={"Width": [800, 1200]})`. It replaces the whole curve (at least 2 points, relative to the spline component) and rebuilds the stamp. A point's `type` defaults to `Curve`; `arriveTangent` and `leaveTangent` apply only to `CurveCustomTangent`, which needs both. `metadata` holds one value per point for each spline parameter of the stamp's graph: a number, `{x,y}` or `{x,y,z}` by type. A parameter left out keeps its values while the point count is unchanged and otherwise takes its default. `voxel(action="voxel_spline_read", actorPath=...)` returns the points, the spline parameters and their per-point values. PCG's Create Voxel Spline builds spline stamps from spline data instead.

## 4. Caves and overhangs

A height layer cannot overhang. Carve and add in a volume layer above it:

1. Make sure the stack has a volume layer above the height layers (the default stack has one).
2. A stamp with `kind: "volume_graph"` (or `mesh`) on that layer: `blendMode: "Subtractive"` carves, `Additive` adds rock.
3. Check with `voxel(action="voxel_query_layer", layerKind="volume", stack=..., layer=..., points=[{x,y,z}])`: `distance` is negative inside solid, positive in air.
4. A `UVoxelNoClippingComponent` teleports its owner back to its last valid location when it ends up inside the volume layer (while `autoAdjustPlayer`, default true; one tick late): add it to the actor to protect with `voxel(action="voxel_component_add", kind="no_clipping")` and point it at the layer with `voxel(action="voxel_no_clipping_set_layer")`.

## 5. Surfaces

The height graph's output `SurfaceType` pin decides which surface lands where (build blends with `Make Surface Type Blend` and `Blend Surface Types`); a stamp parameter of surface type lets each stamp choose. A surface renders only when it is in the world's mega material (`voxel_mega_material_set_surfaces`, `mode: "append"` to add). Smart surface types pick a surface from a graph at each point: `voxel(action="voxel_smart_surface_set", assetPath=..., graph=..., parameters={...})`.

## 6. Sculpting

`voxel(action="voxel_actor_spawn", kind="height_sculpt")` then `voxel(action="voxel_height_sculpt", op="sculpt_height", center={x,y}, radius=..., strength=...)` (`flatten`, `smooth`, `paint_surface`, `apply_graph`, `clear_data`). Volume: `kind: "volume_sculpt"` and `voxel(action="voxel_volume_sculpt")` (`sphere`, `cube`, `flatten`, `smooth`, `surface`, `angle`, `paint`, `apply_graph`). A sculpt actor needs a voxel world whose stack holds its layer, or a `StackOverride` on its stamp that holds it; otherwise the call is refused for having no valid layer. A `brush` names its `type` (`Circular`, `Alpha` or `Pattern`, the last two with a `texture`).

Edits wait for completion by default; `wait: false` queues them outside undo and the auto-save. `voxel(action="voxel_sculpt_asset_set", asset=...)` moves the sculpt data into a save asset so it lives outside the level (`load: false` writes the actor's current data into the asset); `voxel(action="voxel_sculpt_asset_get")` reports the bound asset and whether the data is empty.

## 7. Verify

- `voxel_world_status` until `isReady`.
- `voxel(action="voxel_query_layer", stack=..., layer=..., points=[...], querySurface=true)` on the layer your stamps write to: it defaults to Voxel's built-in default stack and layer (`/Voxel/Default`), which may hold none of them. A point no stamp covers reads `null`.
- `voxel(action="voxel_export_to_render_target", bounds=..., renderTarget=...)` writes a height layer region into a float render target, for a heightmap readback or a GPU consumer.
- Capture the viewport with `editor(action="capture_screenshot")`; a call succeeding is not proof the terrain looks right.

## Before reporting done

- [ ] `voxel_shader_hooks_status` reports `allActive`.
- [ ] Every stamp is on the intended layer and the world's stack holds that layer.
- [ ] Every surface the graphs output is in the mega material.
- [ ] Heights or distances were sampled on the stamps' own stack and layer, and the result was seen in a capture.
