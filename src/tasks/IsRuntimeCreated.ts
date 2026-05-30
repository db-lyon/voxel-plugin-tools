import { InvokeVoxelWorldFunction } from "./InvokeVoxelWorldFunction.js";

/**
 * Read `AVoxelWorld::IsRuntimeCreated()` — whether the voxel runtime is
 * initialized (distinct from fully-rendered; see `voxel.is_world_ready`).
 * Result at `data.returnValues.ReturnValue` as `"true"`/`"false"`.
 *
 * Header: `Voxel/Public/VoxelWorld.h` (`bool IsRuntimeCreated()`).
 */
export default class IsRuntimeCreated extends InvokeVoxelWorldFunction {
  get taskName(): string { return "voxel.is_runtime_created"; }
  protected readonly functionName = "IsRuntimeCreated";
}
