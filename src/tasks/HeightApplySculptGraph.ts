import { HeightSculptOp, type HeightSculptOptions } from "./HeightSculptOp.js";
import { pyVec2, pyFloat, pyStr, type Vec2 } from "./_python.js";

interface Options extends HeightSculptOptions {
  center: Vec2;
  /** Object path to a UVoxelHeightSculptGraph asset. */
  graph: string;
  radius?: number;
}

/**
 * Atomic. Apply a height sculpt graph at a location on an `AVoxelHeightSculptActor`
 * (centre is 2D). The `FVoxelHeightSculptGraphWrapper` is built from the graph
 * asset via positional struct construction.
 * `VoxelHeightSculptBlueprintLibrary::ApplySculptGraph`. Wraps editor.execute_python.
 */
export default class HeightApplySculptGraph extends HeightSculptOp<Options> {
  get taskName(): string { return "voxel.height_apply_sculpt_graph"; }
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
      `_graph = unreal.VoxelHeightSculptGraphWrapper(_ga)`,
    ];
  }

  protected callArgs(): string[] {
    const o = this.options;
    return [
      pyVec2(o.center),
      `radius=${pyFloat(o.radius ?? 500)}`,
      `graph=_graph`,
    ];
  }
}
