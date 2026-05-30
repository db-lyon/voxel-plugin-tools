import { VolumeSculptOp, type VolumeSculptOptions } from "./VolumeSculptOp.js";
import { pyVec3, pyFloat, pyEnum, SCULPT_MODE, type Vec3 } from "./_python.js";

interface Options extends VolumeSculptOptions {
  center: Vec3;
  radius?: number;
  strength?: number;
  mode?: "add" | "remove";
}

/**
 * Atomic. Progressively sculpt a voxel surface on an `AVoxelVolumeSculptActor`
 * (brush left at default). `VoxelVolumeSculptBlueprintLibrary::SculptSurface`.
 * Wraps editor.execute_python.
 */
export default class VolumeSculptSurface extends VolumeSculptOp<Options> {
  get taskName(): string { return "voxel.volume_sculpt_surface"; }
  protected readonly functionName = "sculpt_surface";

  protected validate(): void {
    super.validate();
    if (!this.options.center) throw new Error("center is required");
  }

  protected callArgs(): string[] {
    const o = this.options;
    return [
      pyVec3(o.center),
      `radius=${pyFloat(o.radius ?? 500)}`,
      `strength=${pyFloat(o.strength ?? 0.5)}`,
      `mode=${pyEnum("VoxelSculptMode", o.mode ?? "add", SCULPT_MODE)}`,
    ];
  }
}
