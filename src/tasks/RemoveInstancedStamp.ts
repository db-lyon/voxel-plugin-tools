import { InstancedStampComponentOp, type InstancedCompOptions } from "./InstancedStampComponentOp.js";

interface Options extends InstancedCompOptions {
  /** Index of the stamp to remove. */
  index: number;
}

/** Remove one stamp (by index) from a UVoxelInstancedStampComponent and refresh. */
export default class RemoveInstancedStamp extends InstancedStampComponentOp<Options> {
  get taskName(): string { return "voxel.remove_instanced_stamp"; }

  protected validate(): void {
    super.validate();
    if (typeof this.options.index !== "number") throw new Error("index (number) is required");
  }

  protected bodyLines(): string[] {
    return [
      `_comp.remove_stamp(${this.options.index})`,
      `_comp.update_all_stamps()`,
      `print("voxel.remove_instanced_stamp: removed #${this.options.index}")`,
    ];
  }
}
