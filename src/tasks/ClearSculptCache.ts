import { InvokeVoxelWorldFunction } from "./InvokeVoxelWorldFunction.js";

/**
 * Atomic. Call `AVoxelSculptActorBase::ClearSculptCache()` on a height or volume
 * sculpt actor — frees cached sculpt memory (subsequent edits a touch slower).
 * Distinct from clearing sculpt *data*; this only drops the cache. No return value.
 *
 * Header: `Voxel/Public/Sculpt/VoxelSculptActorBase.h` (`void ClearSculptCache()`).
 */
export default class ClearSculptCache extends InvokeVoxelWorldFunction {
  get taskName(): string { return "voxel.clear_sculpt_cache"; }
  protected readonly functionName = "ClearSculptCache";
}
