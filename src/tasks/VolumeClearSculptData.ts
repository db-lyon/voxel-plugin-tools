import { VolumeSculptOp, type VolumeSculptOptions } from "./VolumeSculptOp.js";

/**
 * Clear all sculpt data on an `AVoxelVolumeSculptActor` (resets edits).
 * `VoxelVolumeSculptBlueprintLibrary::ClearSculptData`. Wraps editor.execute_python.
 */
export default class VolumeClearSculptData extends VolumeSculptOp<VolumeSculptOptions> {
  get taskName(): string { return "voxel.volume_clear_sculpt_data"; }
  protected readonly functionName = "clear_sculpt_data";
  protected callArgs(): string[] { return []; }
}
