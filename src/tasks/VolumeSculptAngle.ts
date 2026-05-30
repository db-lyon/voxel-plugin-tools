import { VolumeSculptOp, type VolumeSculptOptions } from "./VolumeSculptOp.js";
import { pyVec3, pyFloat, pyEnum, SDF_MERGE_MODE, type Vec3 } from "./_python.js";

interface Options extends VolumeSculptOptions {
  center: Vec3;
  radius?: number;
  strength?: number;
  planePoint?: Vec3;
  planeNormal?: Vec3;
  mergeMode?: "intersection" | "override" | "union";
}

/**
 * Move a voxel surface toward a plane (sculpt angles) on an
 * `AVoxelVolumeSculptActor` (brush left at default).
 * `VoxelVolumeSculptBlueprintLibrary::SculptAngle`. Wraps editor.execute_python.
 */
export default class VolumeSculptAngle extends VolumeSculptOp<Options> {
  get taskName(): string { return "voxel.volume_sculpt_angle"; }
  protected readonly functionName = "sculpt_angle";

  protected validate(): void {
    super.validate();
    if (!this.options.center) throw new Error("center is required");
  }

  protected callArgs(): string[] {
    const o = this.options;
    return [
      pyVec3(o.center),
      `radius=${pyFloat(o.radius ?? 500)}`,
      `strength=${pyFloat(o.strength ?? 1)}`,
      `plane_point=${pyVec3(o.planePoint ?? { x: 0, y: 0, z: 0 })}`,
      `plane_normal=${pyVec3(o.planeNormal ?? { x: 0, y: 0, z: 1 })}`,
      `merge_mode=${pyEnum("VoxelSDFMergeMode", o.mergeMode ?? "override", SDF_MERGE_MODE)}`,
    ];
  }
}
