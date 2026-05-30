import { InvokeVoxelWorldFunction } from "./InvokeVoxelWorldFunction.js";

/**
 * Read `AVoxelWorld::IsProcessingNewState()` — whether a state update
 * (regeneration) is in flight. Result at `data.returnValues.ReturnValue` as
 * `"true"`/`"false"`.
 *
 * Header: `Voxel/Public/VoxelWorld.h` (`bool IsProcessingNewState()`).
 */
export default class IsProcessingNewState extends InvokeVoxelWorldFunction {
  get taskName(): string { return "voxel.is_processing_new_state"; }
  protected readonly functionName = "IsProcessingNewState";
}
