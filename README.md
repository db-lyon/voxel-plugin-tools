# ue-mcp-plugin-voxel-plugin

[Voxel Plugin](https://voxelplugin.com) actions for [ue-mcp](https://github.com/db-lyon/ue-mcp).

## What ships

Each action is cited to the C++ header it wraps. Full API reference under [`docs/`](docs/).

| Category | Action                          | Wraps |
|----------|---------------------------------|-------|
| `pcg`    | `voxel_build_scatter_graph`     | `UPCGVoxelSamplerSettings` (`VoxelPCG/Public/PCGVoxelSampler.h`) feeding `UPCGStaticMeshSpawnerSettings` |
| `level`  | `voxel_get_voxel_world_status`  | 5 zero-arg lifecycle UFUNCTIONs on `AVoxelWorld` (`Voxel/Public/VoxelWorld.h`) |

The v0.1.0 release shipped three actions that called ue-mcp tasks with wrong parameter names and passed PCG node-type strings that did not exist; v0.1.1 removed them.

## Install

```bash
ue-mcp plugin install ue-mcp-plugin-voxel-plugin
```

The CLI adds an entry under `plugins:` in your `ue-mcp.yml`. Restart ue-mcp; the injected action shows up under `pcg`.

## Usage

```text
pcg(action="voxel_build_scatter_graph",
    assetPath="/Game/PCG/RockScatter",
    meshes=[
      {mesh: "/Game/Foliage/Rock_A.Rock_A", weight: 2},
      {mesh: "/Game/Foliage/Rock_B.Rock_B"}
    ],
    pointsPerSquaredMeter=0.05,
    seed=42)
```

The call creates a `UPCGGraph` at `assetPath`, adds a Voxel Sampler → Static Mesh Spawner pipeline, and populates the weighted mesh table. Attach the resulting graph to a PCG component on or near your `AVoxelWorld`, then:

```text
pcg(action="execute", actorLabel="MyPCGActor")
```

Check the voxel world is actually live before scattering / stamping into it:

```text
level(action="voxel_get_voxel_world_status", actorLabel="MyVoxelWorld")
```

Returns `{ isRuntimeCreated, isVoxelWorldReady, isProcessingNewState, progress, numPendingTasks }`. The runtime can be mid-regeneration even after `IsRuntimeCreated` flips true — wait for `isVoxelWorldReady && !isProcessingNewState` before pipelines that depend on stable terrain.

## Requirements

- ue-mcp `>= 1.0.15`
- Voxel Plugin enabled in your `.uproject` (`Plugins[].Name == "Voxel"`)

## Develop

```bash
git clone https://github.com/db-lyon/ue-mcp-plugin-voxel-plugin.git
cd ue-mcp-plugin-voxel-plugin
npm install
npm run build
```

## License

MIT - see `LICENSE`.
