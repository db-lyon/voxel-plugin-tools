import { HeightSculptOp, type HeightSculptOptions } from "./HeightSculptOp.js";
import { pyVec2, pyFloat, pyEnum, LEVEL_TOOL_TYPE, type Vec2 } from "./_python.js";

interface Options extends HeightSculptOptions {
  center: Vec2;
  radius?: number;
  falloff?: number;
  type?: "additive" | "subtractive" | "both";
  targetHeight?: number;
}

/**
 * Flatten a height region toward a target height on an
 * `AVoxelHeightSculptActor` (centre is 2D).
 * `VoxelHeightSculptBlueprintLibrary::Flatten`. Wraps editor.execute_python.
 */
export default class HeightFlatten extends HeightSculptOp<Options> {
  get taskName(): string { return "voxel.height_flatten"; }
  protected readonly functionName = "flatten";

  protected validate(): void {
    super.validate();
    if (!this.options.center) throw new Error("center is required");
  }

  protected callArgs(): string[] {
    const o = this.options;
    return [
      pyVec2(o.center),
      `radius=${pyFloat(o.radius ?? 500)}`,
      `falloff=${pyFloat(o.falloff ?? 0.1)}`,
      `type=${pyEnum("VoxelLevelToolType", o.type ?? "additive", LEVEL_TOOL_TYPE)}`,
      `target_height=${pyFloat(o.targetHeight ?? 0)}`,
    ];
  }
}
