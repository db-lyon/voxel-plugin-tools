import { VolumeSculptOp, type VolumeSculptOptions } from "./VolumeSculptOp.js";
import { pyVec3, pyFloat, type Vec3 } from "./_python.js";

interface Options extends VolumeSculptOptions {
  center: Vec3;
  radius?: number;
  strength?: number;
}

/**
 * Atomic. Smooth/blur a region on an `AVoxelVolumeSculptActor`. The optional
 * brush param is left at its default (circular).
 * `VoxelVolumeSculptBlueprintLibrary::Smooth`. Wraps editor.execute_python.
 */
export default class VolumeSmooth extends VolumeSculptOp<Options> {
  get taskName(): string { return "voxel.volume_smooth"; }
  protected readonly functionName = "smooth";

  protected validate(): void {
    super.validate();
    if (!this.options.center) throw new Error("center is required");
  }

  protected callArgs(): string[] {
    const o = this.options;
    return [
      pyVec3(o.center),
      `radius=${pyFloat(o.radius ?? 1000)}`,
      `strength=${pyFloat(o.strength ?? 1)}`,
    ];
  }
}
