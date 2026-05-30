import { InvokeVoxelWorldFunction } from "./InvokeVoxelWorldFunction.js";

/**
 * Atomic. Call `AVoxelWorld::CreateRuntime()` — explicitly initialize the voxel
 * runtime (e.g. when `bCreateRuntimeOnBeginPlay` is off). No return value.
 *
 * Header: `Voxel/Public/VoxelWorld.h` (`void CreateRuntime()`).
 */
export default class CreateRuntime extends InvokeVoxelWorldFunction {
  get taskName(): string { return "voxel.create_runtime"; }
  protected readonly functionName = "CreateRuntime";
}
