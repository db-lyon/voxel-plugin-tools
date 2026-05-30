import { HeightSculptOp, type HeightSculptOptions } from "./HeightSculptOp.js";
import { pyVec2, pyFloat, pyStr, pyEnum, SCULPT_MODE, type Vec2 } from "./_python.js";

interface Options extends HeightSculptOptions {
  center: Vec2;
  radius?: number;
  strength?: number;
  mode?: "add" | "remove";
  /** Optional object path to a UVoxelSurfaceTypeInterface asset to paint. */
  surfaceType?: string;
}

/**
 * Paint a surface type on a height sculpt stamp of an
 * `AVoxelHeightSculptActor` (centre is 2D; metadata + brush at defaults).
 * `VoxelHeightSculptBlueprintLibrary::PaintSurface`. Wraps editor.execute_python.
 */
export default class HeightPaintSurface extends HeightSculptOp<Options> {
  get taskName(): string { return "voxel.height_paint_surface"; }
  protected readonly functionName = "paint_surface";

  protected validate(): void {
    super.validate();
    if (!this.options.center) throw new Error("center is required");
  }

  protected callArgs(): string[] {
    const o = this.options;
    const args = [
      pyVec2(o.center),
      `radius=${pyFloat(o.radius ?? 500)}`,
      `strength=${pyFloat(o.strength ?? 0.05)}`,
      `mode=${pyEnum("VoxelSculptMode", o.mode ?? "add", SCULPT_MODE)}`,
    ];
    if (o.surfaceType) {
      args.push(`surface_type_to_paint=unreal.load_asset(${pyStr(o.surfaceType)})`);
    }
    return args;
  }
}
