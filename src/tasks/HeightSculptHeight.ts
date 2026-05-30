import { HeightSculptOp, type HeightSculptOptions } from "./HeightSculptOp.js";
import { pyVec2, pyFloat, pyEnum, SCULPT_MODE, type Vec2 } from "./_python.js";

interface Options extends HeightSculptOptions {
  center: Vec2;
  radius?: number;
  strength?: number;
  mode?: "add" | "remove";
}

/**
 * Atomic. Raise/lower height on an `AVoxelHeightSculptActor` (centre is 2D).
 * `VoxelHeightSculptBlueprintLibrary::SculptHeight`. Wraps editor.execute_python.
 */
export default class HeightSculptHeight extends HeightSculptOp<Options> {
  get taskName(): string { return "voxel.height_sculpt_height"; }
  protected readonly functionName = "sculpt_height";

  protected validate(): void {
    super.validate();
    if (!this.options.center) throw new Error("center is required");
  }

  protected callArgs(): string[] {
    const o = this.options;
    return [
      pyVec2(o.center),
      `radius=${pyFloat(o.radius ?? 500)}`,
      `strength=${pyFloat(o.strength ?? 0.5)}`,
      `mode=${pyEnum("VoxelSculptMode", o.mode ?? "add", SCULPT_MODE)}`,
    ];
  }
}
