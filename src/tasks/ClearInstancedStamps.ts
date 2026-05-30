import { InstancedStampComponentOp, type InstancedCompOptions } from "./InstancedStampComponentOp.js";

/** Remove every stamp from a UVoxelInstancedStampComponent and refresh. */
export default class ClearInstancedStamps extends InstancedStampComponentOp<InstancedCompOptions> {
  get taskName(): string { return "voxel.clear_instanced_stamps"; }
  protected bodyLines(): string[] {
    return [`_comp.clear_stamps()`, `_comp.update_all_stamps()`, `print("voxel.clear_instanced_stamps: cleared")`];
  }
}
