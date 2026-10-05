# voxel-plugin-tools

[Voxel Plugin 2](https://voxelplugin.com) for [ue-mcp](https://github.com/db-lyon/ue-mcp): 38 native actions in a `voxel` category covering worlds, stamps, sculpting, layer queries, voxel assets, voxel PCG nodes and graph authoring.

## Install

```bash
ue-mcp plugin install voxel
```

This adds the plugin to `ue-mcp.yml`, copies the `VoxelPluginTools` C++ module into `Plugins/`, and installs the `voxel-terrain` skill. Rebuild the project, then restart ue-mcp.

## Requirements

- ue-mcp 1.3.8 or later (enforced by `minServerVersion`; prerelease servers included)
- Voxel Plugin 2 enabled in the `.uproject`; built and tested against the `dev` branch
- Unreal Engine 5.8

## Usage

```text
voxel(action="voxel_asset_create", type="height_graph", name="HG_Terrain", packagePath="/Game/Terrain")
voxel(action="voxel_world_spawn", label="World", megaMaterial="/Game/Terrain/MM_Terrain")
voxel(action="voxel_actor_spawn", kind="stamp", label="Terrain")
voxel(action="voxel_stamp_set", actorPath="<from the spawn>", kind="height_graph",
      asset="/Game/Terrain/HG_Terrain", parameters={Amplitude: 3000})
```

The `voxel-terrain` skill walks through the full workflow. `ue-mcp.plugin.yml` is the reference for every action and parameter.

## Actions

| Area | Actions |
|---|---|
| World | `voxel_shader_hooks_status`, `voxel_world_spawn`, `voxel_world_configure`, `voxel_world_status`, `voxel_world_runtime` |
| Actors | `voxel_actor_spawn` (stamp, height/volume sculpt, collision baker, debug), `voxel_component_add`, `voxel_no_clipping_set_layer` |
| Stamps | `voxel_stamp_set` (height/volume graph, heightmap, mesh, height/volume spline), `voxel_stamp_read`, `voxel_stamp_set_parameters`, `voxel_instanced_stamps` |
| Sculpting | `voxel_height_sculpt`, `voxel_volume_sculpt`, `voxel_sculpt_asset_get`, `voxel_sculpt_asset_set` |
| Queries | `voxel_query_layer`, `voxel_export_to_render_target` |
| Assets | `voxel_asset_create`, `voxel_asset_set_property`, `voxel_mega_material_set_surfaces`, `voxel_surface_type_set`, `voxel_smart_surface_set`, `voxel_layer_stack_set` |
| PCG | `voxel_pcg_add_node`, `voxel_pcg_configure_sampler` |
| Graphs | `voxel_graph_read`, `voxel_graph_list_node_types`, `voxel_graph_add_node`, `voxel_graph_connect`, `voxel_graph_disconnect`, `voxel_graph_set_pin_default`, `voxel_graph_delete_node`, `voxel_graph_export_t3d`, `voxel_graph_import_t3d`, `voxel_graph_add_parameter`, `voxel_graph_remove_parameter`, `voxel_graph_set_parameter_default` |

## Conventions

- Actors are addressed by `actorPath` (stable) or `actorLabel`; stamp actors relabel themselves, so prefer the path.
- Rotations are `{pitch,yaw,roll}`, vectors `{x,y,z}` in centimetres.
- Content packages an action dirties are saved before it replies (`autoSaved` in the result); levels are left to you.
- Existing assets are never overwritten.
- Non-finite numbers in results are reported as `null`.

## Develop

See [CONTRIBUTING.md](CONTRIBUTING.md). `npm run check` validates the manifest against the host schema and the C++ registrations; `node scripts/live-test.mjs` runs every action against a live editor.

## License

MIT. See [LICENSE](LICENSE).
