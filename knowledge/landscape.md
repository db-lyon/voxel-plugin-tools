# Voxel Plugin - Landscape actions

This plugin contributes one landscape-side action for [Voxel Plugin](https://voxelplugin.com):

- `landscape(action="voxel_bake_heightmap", ...)` - bake a region of a voxel terrain into a standard UE Landscape heightmap.

## When to use

Use it when the user wants to convert a voxel terrain (or a region of it) into a standard Landscape actor for the parts of the workflow that need it: high-quality runtime LOD, foliage painting, landscape materials, etc.

The reverse conversion (Landscape → voxel) is not part of this plugin.

## Typical sequence

```
landscape(action="voxel_bake_heightmap",
          landscapeLabel="Landscape",
          bounds={ min: {x: 0,    y: 0},
                   max: {x: 8064, y: 8064} },
          resolution=1009)
```

The action samples voxel heights on a `resolution x resolution` grid spanning `bounds`, writes a raw uint16 heightmap to `Intermediate/Voxel/voxel_heightmap.r16`, then imports it into the target landscape via the built-in `landscape.import_heightmap` action.

## Parameters

- `landscapeLabel` (required) - the in-editor label of the target Landscape actor.
- `bounds` (required) - `{ min: {x, y}, max: {x, y} }` in world centimetres.
- `resolution` (optional, default 1009) - heightmap side length in samples. Use a UE-friendly value: 127, 253, 505, 1009, 2017, 4033, 8129.

## Notes

- The Python escape hatch is used here because Voxel Plugin exposes its voxel heights to Python but not to PCG or the C++ bridge directly. The script is defensive: if `VoxelBlueprintLibrary` is not loaded it returns a clear error rather than a traceback.
- Height mapping uses a centred uint16 range (offset +32768). If your voxel volume uses a different scale, adjust the script or wrap this action.
