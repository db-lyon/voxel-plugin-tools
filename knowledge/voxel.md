# voxel: Voxel Plugin 2

Every action is a native C++ handler; nothing runs through Python. Three skills carry the workflows: `voxel-plugin-tools-voxel-terrain` (worlds, stamps, surfaces, sculpting), `voxel-plugin-tools-voxel-graphs` (graph authoring) and `voxel-plugin-tools-voxel-pcg` (PCG on voxel terrain).

Before material work run `voxel_shader_hooks_status`: Voxel materials render only with Voxel's engine shader patches applied. Without them terrain shows a gray grid checker and nothing logs an error; the result names the fix. The patch lives in the engine install, so anything that rewrites engine files (an engine hotfix, a Fab plugin install) removes it; recheck after any of them.

Conventions:
- Every action holds its call to a typed C++ contract (`describe` shows it): an unknown key, a wrong JSON type, a fractional integer, an out-of-range number or an enum value spelt differently is refused, never coerced or ignored. Ops and kinds take only their own fields: `voxel_volume_sculpt` `op: "cube"` refuses `radius`, a height stamp refuses `boundsExtensionMultiplier`.
- Actors: pass `actorPath` from the spawn result, or `actorLabel`, never both. Stamp actors relabel themselves from their stamp, so labels go stale.
- Rotations are `{pitch,yaw,roll}` and vectors `{x,y,z}` in centimetres, every component a number.
- Content packages an edit dirties are saved before the reply (listed in `autoSaved`; asset and graph edits also report `saved`); every mutating action takes `save: false`. Levels are never saved for you.
- An empty string is never a synonym for omitting a field: omit `stack`, `layer`, `componentName` or `terminalGraph` to get the default. Where `""` means something it is documented: it clears an object reference, and detaches a sculpt asset.
- `voxel_query_layer` and the PCG samplers default to Voxel's built-in default stack and layer (`/Voxel/Default`); pass the ones your stamps write to.
- Sculpt brushes name their `type` (`Circular`, `Alpha`, `Pattern`), which decides the fields they take; set the brush falloff with `falloff` or `brush.falloffAmount`, not both.
- Existing assets are never overwritten: `voxel_asset_create` returns them with `existed: true`.
- `voxel_asset_set_property` sets any other top-level property of an asset (UE import text, details-panel semantics) when no dedicated action covers it.
- `instanceOf` on `voxel_asset_create` makes a graph instance: it inherits the base graph's nodes and overrides its parameters with `voxel_graph_set_parameter_default`. Node and parameter edits on an instance are refused; edit the base.

Terrain in six calls:
1. `voxel(action="voxel_asset_create", type="height_graph", name="HG_Terrain", packagePath="/Game/Terrain")`: Voxel's template, Advanced Noise 2D into Output Height, no parameters yet
2. `voxel(action="voxel_asset_create", type="mega_material", name="MM_Terrain", packagePath="/Game/Terrain")`
3. `voxel(action="voxel_world_spawn", label="World", megaMaterial="/Game/Terrain/MM_Terrain")`
4. `voxel(action="voxel_actor_spawn", kind="stamp", label="Terrain")` returns `actorPath`
5. `voxel(action="voxel_stamp_set", actorPath=..., kind="height_graph", asset="/Game/Terrain/HG_Terrain")`
6. `voxel(action="voxel_query_layer", points=[{x:0,y:0}])` to read heights back

Graph editing: `voxel_graph_list_node_types` (query words) gives `nodeType` keys; `voxel_graph_add_parameter`, then place its getter with `voxel_graph_add_node` `nodeType="Parameters|<name>"`; stamps override parameters by name (`voxel_stamp_set` `parameters`, `voxel_stamp_set_parameters`). Functions: `voxel_graph_add_function` declares one with its inputs and outputs and returns its `terminalGraph` GUID; a function library's exposed functions are node types in every graph.

Spline stamps: `voxel_spline_set_points` sets the curve and per-point spline parameter values (core `set_spline_points` refuses Voxel's spline); `voxel_spline_read` reads them back.

Surfaces: `voxel_surface_type_set` (material), `voxel_mega_material_set_surfaces` (list on the mega material), and the height graph's output `SurfaceType` pin decides where each one lands.

PCG: create the engine graph with `pcg(action="create_graph")`, then `voxel_pcg_add_node` (`wait_for_world`, `sampler_v2`, ...) and `voxel_pcg_configure_sampler`, naming the height layer explicitly (a new sampler reads the default volume layer). One `sampler_v2` call on a height layer returns empty, silently, past 1,048,576 candidate positions; for larger areas use a partitioned, runtime-generated component, never a Loop over cells. The PCG skill has the rest.

Object-typed values (surface types, assets) accept `/Game/Path/Asset` or `/Game/Path/Asset.Asset`; a value that does not resolve is an error.
