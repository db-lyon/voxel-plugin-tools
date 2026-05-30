import { VolumeSculptOp, type VolumeSculptOptions } from "./VolumeSculptOp.js";
import { pyVec3, pyFloat, pyStr, pyEnum, SCULPT_MODE, type Vec3 } from "./_python.js";

interface Options extends VolumeSculptOptions {
  center: Vec3;
  radius?: number;
  strength?: number;
  mode?: "add" | "remove";
  /** Optional object path to a UVoxelSurfaceTypeInterface asset to paint. */
  surfaceType?: string;
}

/**
 * Atomic. Paint a surface type on a sculpt stamp of an `AVoxelVolumeSculptActor`
 * (metadata + brush left at defaults). `VoxelVolumeSculptBlueprintLibrary::PaintSurface`.
 * Wraps editor.execute_python.
 */
export default class VolumePaintSurface extends VolumeSculptOp<Options> {
  get taskName(): string { return "voxel.volume_paint_surface"; }
  protected readonly functionName = "paint_surface";

  protected validate(): void {
    super.validate();
    if (!this.options.center) throw new Error("center is required");
  }

  protected callArgs(): string[] {
    const o = this.options;
    const args = [
      pyVec3(o.center),
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
