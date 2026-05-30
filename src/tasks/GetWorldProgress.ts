import { InvokeVoxelWorldFunction } from "./InvokeVoxelWorldFunction.js";

/**
 * Atomic. Read `AVoxelWorld::GetProgress()` — 0..1 while a state is processing,
 * 1 when idle. Result at `data.returnValues.ReturnValue` (float as string).
 *
 * Header: `Voxel/Public/VoxelWorld.h` (`float GetProgress()`).
 */
export default class GetWorldProgress extends InvokeVoxelWorldFunction {
  get taskName(): string { return "voxel.get_world_progress"; }
  protected readonly functionName = "GetProgress";
}
