import { HeightSculptOp, type HeightSculptOptions } from "./HeightSculptOp.js";

/**
 * Atomic. Clear all sculpt data on an `AVoxelHeightSculptActor` (resets edits).
 * `VoxelHeightSculptBlueprintLibrary::ClearSculptData`. Wraps editor.execute_python.
 */
export default class HeightClearSculptData extends HeightSculptOp<HeightSculptOptions> {
  get taskName(): string { return "voxel.height_clear_sculpt_data"; }
  protected readonly functionName = "clear_sculpt_data";
  protected callArgs(): string[] { return []; }
}
