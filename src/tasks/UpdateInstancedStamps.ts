import { InstancedStampComponentOp, type InstancedCompOptions } from "./InstancedStampComponentOp.js";

/** Refresh all stamps on a UVoxelInstancedStampComponent (apply pending edits). */
export default class UpdateInstancedStamps extends InstancedStampComponentOp<InstancedCompOptions> {
  get taskName(): string { return "voxel.update_instanced_stamps"; }
  protected bodyLines(): string[] {
    return [`_comp.update_all_stamps()`, `print("voxel.update_instanced_stamps: updated")`];
  }
}
