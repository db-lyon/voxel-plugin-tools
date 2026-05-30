import { InvokeVoxelWorldFunction } from "./InvokeVoxelWorldFunction.js";

/**
 * Call `AVoxelWorld::DestroyRuntime()` — tear down the voxel runtime
 * (frees meshes/collision/nav until re-created). No return value.
 *
 * Header: `Voxel/Public/VoxelWorld.h` (`void DestroyRuntime()`).
 */
export default class DestroyRuntime extends InvokeVoxelWorldFunction {
  get taskName(): string { return "voxel.destroy_runtime"; }
  protected readonly functionName = "DestroyRuntime";
}
