import { SculptOp, type SculptActorOptions } from "./SculptOp.js";

/**
 * Free the sculpt cache on an `AVoxelVolumeSculptActor` (subsequent
 * edits a touch slower; does NOT clear sculpt data — see volume_clear_sculpt_data).
 *
 * This is a static library function (NOT an actor method — `invoke_function`
 * can't reach it), and it lives on the base `VoxelVolumeSculptBlueprintLibrary`,
 * not the `_BlueprintOnly` wrapper. Wraps editor.execute_python.
 */
export default class VolumeClearSculptCache extends SculptOp<SculptActorOptions> {
  get taskName(): string { return "voxel.volume_clear_sculpt_cache"; }
  protected readonly libraryClass = "VoxelVolumeSculptBlueprintLibrary";
  protected readonly functionName = "clear_sculpt_cache";
  protected callArgs(): string[] { return []; }
}
