import { VolumeSculptOp, type VolumeSculptOptions } from "./VolumeSculptOp.js";
import { pyVec3, pyFloat, pyEnum, LEVEL_TOOL_TYPE, type Vec3 } from "./_python.js";

interface Options extends VolumeSculptOptions {
  center: Vec3;
  normal?: Vec3;
  radius?: number;
  height?: number;
  falloff?: number;
  type?: "additive" | "subtractive" | "both";
}

/**
 * Flatten a voxel surface along a normal on an `AVoxelVolumeSculptActor`.
 * `VoxelVolumeSculptBlueprintLibrary::Flatten`. Wraps editor.execute_python.
 */
export default class VolumeFlatten extends VolumeSculptOp<Options> {
  get taskName(): string { return "voxel.volume_flatten"; }
  protected readonly functionName = "flatten";

  protected validate(): void {
    super.validate();
    if (!this.options.center) throw new Error("center is required");
  }

  protected callArgs(): string[] {
    const o = this.options;
    return [
      pyVec3(o.center),
      `normal=${pyVec3(o.normal ?? { x: 0, y: 0, z: 1 })}`,
      `radius=${pyFloat(o.radius ?? 500)}`,
      `height=${pyFloat(o.height ?? 1000)}`,
      `falloff=${pyFloat(o.falloff ?? 0.1)}`,
      `type=${pyEnum("VoxelLevelToolType", o.type ?? "additive", LEVEL_TOOL_TYPE)}`,
    ];
  }
}
