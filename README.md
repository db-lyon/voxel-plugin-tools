# voxel-plugin-tools

[Voxel Plugin](https://voxelplugin.com) actions for [ue-mcp](https://github.com/db-lyon/ue-mcp).

## Design

**Every action is atomic — exactly one host ue-mcp call per task.** Orchestrations (spawn-then-configure, build-this-PCG-graph, splice-a-node) ship as **flows**, not as composite tasks. That keeps each action small, predictable, and rollback-friendly; the orchestration layer is where multi-step semantics live.

Full Voxel Plugin API reference under [`docs/`](docs/).

## What ships

### Atomic actions

| Category | Action                 | Wraps                                                  | Header |
|----------|------------------------|--------------------------------------------------------|--------|
| `level`  | `voxel_spawn_world`    | `level.place_actor` with `/Script/Voxel.VoxelWorld`    | `Voxel/Public/VoxelWorld.h` |
| `level`  | `voxel_is_world_ready` | `editor.invoke_function IsVoxelWorldReady`             | `Voxel/Public/VoxelWorld.h` |

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
