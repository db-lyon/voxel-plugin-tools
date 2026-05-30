import { InvokeVoxelWorldFunction } from "./InvokeVoxelWorldFunction.js";

/**
 * Read `AVoxelWorld::GetNumPendingTasks()` — count of queued background
 * tasks; a coarse "how much work is left" signal. Result at
 * `data.returnValues.ReturnValue` (int as string).
 *
 * Header: `Voxel/Public/VoxelWorld.h` (`int32 GetNumPendingTasks()`).
 */
export default class GetNumPendingTasks extends InvokeVoxelWorldFunction {
  get taskName(): string { return "voxel.get_num_pending_tasks"; }
  protected readonly functionName = "GetNumPendingTasks";
}
