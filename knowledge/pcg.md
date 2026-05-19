# Voxel Plugin - PCG actions

This plugin contributes two PCG-side actions for [Voxel Plugin](https://voxelplugin.com) terrains:

- `pcg(action="voxel_scatter_meshes", ...)` - build a graph that scatters static meshes on a voxel terrain via Voxel Sampler.
- `pcg(action="voxel_spawn_stamps", ...)` - build a graph that places voxel stamp assets across a voxel terrain.

## When to use

Reach for these when the user wants procedural placement on a **voxel** terrain. For standard UE Landscape, use the built-in PCG nodes directly.

## Typical sequence

```
pcg(action="create_graph", path="/Game/PCG/MyScatter")    # if no graph exists
pcg(action="voxel_scatter_meshes",
    graphPath="/Game/PCG/MyScatter",
    mesh="/Game/Foliage/Rock01.Rock01",
    density=0.25)
pcg(action="execute", graphPath="/Game/PCG/MyScatter")    # materialise
```

The full setup is also available as a flow:

```
flow(action="run", flowName="voxel_scatter_setup", params={
  graphPath: "/Game/PCG/MyScatter",
  mesh: "/Game/Foliage/Rock01.Rock01"
})
```

## Parameters

`voxel_scatter_meshes`:
- `graphPath` (required) - asset path of the PCG graph to build (created if missing).
- `mesh` (required) - StaticMesh asset path to scatter.
- `density` (optional, default 0.25) - points per square metre at the surface sampler.
- `minScale` / `maxScale` (optional) - uniform scale jitter, default 0.8 .. 1.4.
- `seed` (optional) - deterministic seed for the density filter and transform jitter.

`voxel_spawn_stamps`:
- `graphPath` (required).
- `stampAsset` (required) - Voxel stamp asset path.
- `count` (optional, default 50) - target stamp count; approximated via density.
- `radius` (optional, default 500) - radius of the scatter region in cm.
- `seed` (optional).

## Requirements

The host project must have the Voxel Plugin enabled in its `.uproject` as `Voxel` (the `.uplugin` filename). The plugin installer warns at install time if `Voxel` is missing; the actions themselves will fail with a clear error if the runtime types aren't loaded.
