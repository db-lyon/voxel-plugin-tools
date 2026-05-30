import { InstancedStampComponentOp, type InstancedCompOptions } from "./InstancedStampComponentOp.js";

/** Report the stamp count of a UVoxelInstancedStampComponent on a VOXEL_STAMP_COUNT= line. */
export default class CountInstancedStamps extends InstancedStampComponentOp<InstancedCompOptions> {
  get taskName(): string { return "voxel.count_instanced_stamps"; }
  protected bodyLines(): string[] {
    return [`print("VOXEL_STAMP_COUNT=" + str(_comp.num_stamps()))`];
  }
}
