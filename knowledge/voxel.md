# voxel: Voxel Plugin 2

Every action is a native C++ handler; nothing runs through Python.

Before material work run `voxel_shader_hooks_status`: Voxel materials render only with Voxel's engine shader patches applied. Without them terrain shows a gray grid checker and nothing logs an error; the result names the fix. Epic Launcher updates that write into the engine folder (engine hotfixes, the Fab plugin) restore the stock shaders and silently remove the hooks, so recheck after any of them.

Conventions:
- Actors: pass `actorPath` from the spawn result. Stamp actors relabel themselves from their stamp, so labels go stale.
- Rotations are `{pitch,yaw,roll}`; vectors are `{x,y,z}` in centimetres.
- Content packages an edit dirties are saved before the reply (listed in `autoSaved`); pass `save: false` where a handler takes it. Levels are never saved for you.
- Existing assets are never overwritten: `voxel_asset_create` returns them with `existed: true`.
- Graph assets come from their factory, so a new height graph already has Advanced Noise 2D wired to Output Height.

Terrain in five calls:
1. `voxel(action="voxel_asset_create", type="height_graph", name="HG_Terrain", packagePath="/Game/Terrain")`
2. `voxel(action="voxel_world_spawn", label="World", megaMaterial="/Game/Terrain/MM_Terrain")`
3. `voxel(action="voxel_actor_spawn", kind="stamp", label="Terrain")` returns `actorPath`
4. `voxel(action="voxel_stamp_set", actorPath=..., kind="height_graph", asset="/Game/Terrain/HG_Terrain", parameters={Amplitude: 3000})`
5. `voxel(action="voxel_query_layer", points=[{x:0,y:0}])` to read heights back

Graph editing: `voxel_graph_list_node_types` (query words) gives `nodeType` keys; `voxel_graph_add_parameter` then add its getter with `nodeType="Parameters|<name>"`; stamps override parameters by name.

Surfaces: `voxel_surface_type_set` (material), `voxel_mega_material_set_surfaces` (list on the mega material), and the height graph's Output Height `SurfaceType` pin decides where each one lands.

PCG: create the engine graph with `pcg(action="create_graph")`, then `voxel_pcg_add_node` (`wait_for_world`, `sampler_v2`, ...) and `voxel_pcg_configure_sampler`. `sampler_v2` writes one attribute per surface type; filter on it. One sampler call returns empty, silently, past 1,048,576 candidate positions; for larger areas loop the sampler over grid cells (voxel-terrain skill, section 6).
