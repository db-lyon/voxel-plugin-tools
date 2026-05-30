import { SculptOp, type SculptActorOptions } from "./SculptOp.js";

/**
 * Atomic. Free the sculpt cache on an `AVoxelHeightSculptActor` (subsequent
 * edits a touch slower; does NOT clear sculpt data — see height_clear_sculpt_data).
 *
 * Static library function on the base `VoxelHeightSculptBlueprintLibrary` (not
 * the `_BlueprintOnly` wrapper, not an actor method). Wraps editor.execute_python.
 */
export default class HeightClearSculptCache extends SculptOp<SculptActorOptions> {
  get taskName(): string { return "voxel.height_clear_sculpt_cache"; }
  protected readonly libraryClass = "VoxelHeightSculptBlueprintLibrary";
  protected readonly functionName = "clear_sculpt_cache";
  protected callArgs(): string[] { return []; }
}
