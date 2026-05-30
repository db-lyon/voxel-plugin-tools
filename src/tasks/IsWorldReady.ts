import { InvokeVoxelWorldFunction } from "./InvokeVoxelWorldFunction.js";

/**
 * Read `AVoxelWorld::IsVoxelWorldReady()` — the single most useful gate
 * before scattering / stamping / sampling. Result at
 * `data.returnValues.ReturnValue` as `"true"`/`"false"`.
 *
 * Header: `Voxel/Public/VoxelWorld.h` (`bool IsVoxelWorldReady()`).
 */
export default class IsWorldReady extends InvokeVoxelWorldFunction {
  get taskName(): string { return "voxel.is_world_ready"; }
  protected readonly functionName = "IsVoxelWorldReady";
}
