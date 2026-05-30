import { VolumeSculptOp, type VolumeSculptOptions } from "./VolumeSculptOp.js";
import { pyVec3, pyFloat, pyEnum, SCULPT_MODE, type Vec3 } from "./_python.js";

interface Options extends VolumeSculptOptions {
  center: Vec3;
  radius?: number;
  mode?: "add" | "remove";
  smoothness?: number;
}

/**
 * Add or remove a sphere on an `AVoxelVolumeSculptActor`.
 * `VoxelVolumeSculptBlueprintLibrary::SculptSphere`. Wraps editor.execute_python.
 */
export default class VolumeSculptSphere extends VolumeSculptOp<Options> {
  get taskName(): string { return "voxel.volume_sculpt_sphere"; }
  protected readonly functionName = "sculpt_sphere";

  protected validate(): void {
    super.validate();
    if (!this.options.center) throw new Error("center is required");
  }

  protected callArgs(): string[] {
    const o = this.options;
    return [
      pyVec3(o.center),
      `radius=${pyFloat(o.radius ?? 1000)}`,
      `mode=${pyEnum("VoxelSculptMode", o.mode ?? "add", SCULPT_MODE)}`,
      `smoothness=${pyFloat(o.smoothness ?? 0)}`,
    ];
  }
}
