import { VolumeSculptOp, type VolumeSculptOptions } from "./VolumeSculptOp.js";
import { pyVec3, pyRotator, pyFloat, pyEnum, SCULPT_MODE, type Vec3, type Rot3 } from "./_python.js";

interface Options extends VolumeSculptOptions {
  center: Vec3;
  size?: Vec3;
  rotation?: Rot3;
  roundness?: number;
  mode?: "add" | "remove";
  smoothness?: number;
}

/**
 * Add or remove a (rounded, rotated) cube on an `AVoxelVolumeSculptActor`.
 * `VoxelVolumeSculptBlueprintLibrary::SculptCube`. Wraps editor.execute_python.
 */
export default class VolumeSculptCube extends VolumeSculptOp<Options> {
  get taskName(): string { return "voxel.volume_sculpt_cube"; }
  protected readonly functionName = "sculpt_cube";

  protected validate(): void {
    super.validate();
    if (!this.options.center) throw new Error("center is required");
  }

  protected callArgs(): string[] {
    const o = this.options;
    return [
      pyVec3(o.center),
      `size=${pyVec3(o.size ?? { x: 1000, y: 1000, z: 1000 })}`,
      `rotation=${pyRotator(o.rotation ?? { pitch: 0, yaw: 0, roll: 0 })}`,
      `roundness=${pyFloat(o.roundness ?? 0)}`,
      `mode=${pyEnum("VoxelSculptMode", o.mode ?? "add", SCULPT_MODE)}`,
      `smoothness=${pyFloat(o.smoothness ?? 0)}`,
    ];
  }
}
