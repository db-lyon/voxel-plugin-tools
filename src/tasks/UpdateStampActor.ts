import { InvokeVoxelWorldFunction } from "./InvokeVoxelWorldFunction.js";

/**
 * Atomic. Call `AVoxelStampActor::UpdateStamp()` — re-apply the actor's stamp
 * after its properties change. No return value.
 *
 * Header: `Voxel/Public/VoxelStampActor.h` (`void UpdateStamp()`).
 */
export default class UpdateStampActor extends InvokeVoxelWorldFunction {
  get taskName(): string { return "voxel.update_stamp_actor"; }
  protected readonly functionName = "UpdateStamp";
}
