# voxel-plugin-tools

[Voxel Plugin](https://voxelplugin.com) actions for [ue-mcp](https://github.com/db-lyon/ue-mcp).

## Design

**Every action wraps exactly one host ue-mcp call.** Orchestrations (spawn-then-configure, build-this-PCG-graph, splice-a-node) ship as **flows**, not as composite tasks. That keeps each action small, predictable, and rollback-friendly; the orchestration layer is where multi-step semantics live.

Full Voxel Plugin API reference under [`docs/`](docs/).

## What ships

**60 actions** across three host categories, plus one flow. Each action surfaces as
`<category>(action="voxel_<name>", …)` and cites the C++ header it wraps. The
authoritative list — every action, its parameters, and the header it maps to — is the
manifest, [`ue-mcp.plugin.yml`](ue-mcp.plugin.yml); the full API reference is under
[`docs/`](docs/).

### Actions by group

| Host category | Group | Count | Examples |
|---|---|---:|---|
| `level` | Spawns (`AVoxelWorld`, stamp/sculpt/baker/debug actors) | 6 | `voxel_spawn_world`, `voxel_spawn_volume_sculpt_actor` |
| `level` | World lifecycle & status (`AVoxelWorld`) | 7 | `voxel_is_world_ready`, `voxel_create_runtime` |
| `level` | World config (LayerStack, MegaMaterial, VoxelSize, Nanite/Nav/Lumen, LODQuality) | 7 | `voxel_set_world_layer_stack`, `voxel_set_world_lod_quality` |
| `level` | Volume sculpting (`AVoxelVolumeSculptActor`) | 10 | `voxel_volume_sculpt_sphere`, `voxel_volume_apply_sculpt_graph` |
| `level` | Height sculpting (`AVoxelHeightSculptActor`) | 7 | `voxel_height_sculpt_height`, `voxel_height_flatten` |
| `level` | Stamps (heightmap, graph, mesh, spline + components) | 8 | `voxel_set_heightmap_stamp`, `voxel_set_mesh_stamp` |
| `level` | Sculpt persistence (external save assets) | 2 | `voxel_set_sculpt_save_asset`, `voxel_get_sculpt_save_asset` |
| `level` | Query & export | 3 | `voxel_query_voxel_layer`, `voxel_export_voxel_data_to_render_target` |
| `level` | Instanced stamps (`UVoxelInstancedStampComponent`) | 5 | `voxel_add_instanced_stamp_component`, `voxel_count_instanced_stamps` |
| `level` | No-clipping (`UVoxelNoClippingComponent`) | 2 | `voxel_add_no_clipping_component`, `voxel_set_no_clipping_layer` |
| `asset` | Create / configure Voxel assets | 2 | `voxel_create_asset`, `voxel_set_asset_property` |
| `pcg`   | VoxelPCG nodes | 1 | `voxel_pcg_add_node` (15 node types) |

### Flows

| Flow                 | Sequence |
|----------------------|----------|
| `voxel_hello_world`  | `voxel.spawn_world` → `level.set_actor_property LayerStack` (bundled default) |

## 0-to-1 workflow

```text
flow(action="run", flowName="voxel_hello_world",
     params={ label: "MyVoxelWorld" })
```

Spawns an `AVoxelWorld` and assigns the plugin-bundled `/Voxel/Default/DefaultStack.DefaultStack` so the actor renders. The flow threads the spawned actor's label from step 1 into step 2 via flowkit's `${steps.1.data.actorLabel}` reference.

Poll readiness:

```text
level(action="voxel_is_world_ready", actorLabel="MyVoxelWorld")
# => { ..., returnValues: { ReturnValue: "true" } }
```

The return is the raw `editor.invoke_function` result — parse `data.returnValues.ReturnValue === "true"` on the caller side. Loop on the agent's side; flows don't poll.

## Install

```bash
ue-mcp plugin install voxel-plugin-tools
```

Adds an entry under `plugins:` in your `ue-mcp.yml`. Restart ue-mcp.

## Requirements

- ue-mcp `>= 1.0.15`
- Voxel Plugin enabled in your `.uproject` (`Plugins[].Name == "Voxel"`)

## Develop

```bash
git clone https://github.com/db-lyon/voxel-plugin-tools.git
cd voxel-plugin-tools
npm install
npm run build
```

## License

MIT - see `LICENSE`.
