import { HeightSculptOp, type HeightSculptOptions } from "./HeightSculptOp.js";
import { pyVec2, pyFloat, type Vec2 } from "./_python.js";

interface Options extends HeightSculptOptions {
  center: Vec2;
  radius?: number;
  strength?: number;
}

/**
 * Smooth/blur a height region on an `AVoxelHeightSculptActor` (centre is
 * 2D; brush left at default). `VoxelHeightSculptBlueprintLibrary::Smooth`.
 * Wraps editor.execute_python.
 */
export default class HeightSmooth extends HeightSculptOp<Options> {
  get taskName(): string { return "voxel.height_smooth"; }
  protected readonly functionName = "smooth";

  protected validate(): void {
    super.validate();
    if (!this.options.center) throw new Error("center is required");
  }

  protected callArgs(): string[] {
    const o = this.options;
    return [
      pyVec2(o.center),
      `radius=${pyFloat(o.radius ?? 1000)}`,
      `strength=${pyFloat(o.strength ?? 1)}`,
    ];
  }
}
