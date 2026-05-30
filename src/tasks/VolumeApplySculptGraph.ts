import { VolumeSculptOp, type VolumeSculptOptions } from "./VolumeSculptOp.js";
import { pyVec3, pyRotator, pyFloat, pyStr, type Vec3, type Rot3 } from "./_python.js";

interface Options extends VolumeSculptOptions {
  center: Vec3;
  /** Object path to a UVoxelVolumeSculptGraph asset. */
  graph: string;
  radius?: number;
  rotation?: Rot3;
}

/**
 * Atomic. Apply a volume sculpt graph at a location on an `AVoxelVolumeSculptActor`.
 * The `FVoxelVolumeSculptGraphWrapper` is built from the graph asset via positional
 * struct construction (its make-fn isn't Python-exposed).
 * `VoxelVolumeSculptBlueprintLibrary::ApplySculptGraph`. Wraps editor.execute_python.
 */
export default class VolumeApplySculptGraph extends VolumeSculptOp<Options> {
  get taskName(): string { return "voxel.volume_apply_sculpt_graph"; }
  protected readonly functionName = "apply_sculpt_graph";

  protected validate(): void {
    super.validate();
    if (!this.options.center) throw new Error("center is required");
    if (!this.options.graph) throw new Error("graph (asset path) is required");
  }

  protected setupLines(): string[] {
    const p = pyStr(this.options.graph);
    return [
      `_ga = unreal.load_asset(${p})`,
      `if _ga is None:`,
      `    raise Exception("graph asset not found: " + ${p})`,
      `_graph = unreal.VoxelVolumeSculptGraphWrapper(_ga)`,
    ];
  }

  protected callArgs(): string[] {
    const o = this.options;
    return [
      pyVec3(o.center),
      `radius=${pyFloat(o.radius ?? 500)}`,
      `graph=_graph`,
      `rotation=${pyRotator(o.rotation ?? { pitch: 0, yaw: 0, roll: 0 })}`,
    ];
  }
}
