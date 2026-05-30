import { UeMcpTask, type TaskResult } from "ue-mcp/task";
import { withResolvedActor } from "./_python.js";

interface Options {
  /** Label of a height or volume sculpt actor. */
  actorLabel: string;
}

/**
 * Read the external save asset linked to a sculpt actor — returns its object
 * path (or `None`) on a `VOXEL_SAVE_ASSET=` line. Wraps editor.execute_python.
 *
 * Header: `Voxel/Public/Sculpt/...SculptActor.h` (`GetExternalSaveAsset`).
 */
export default class GetSculptSaveAsset extends UeMcpTask<Options> {
  get taskName(): string { return "voxel.get_sculpt_save_asset"; }

  protected validate(): void {
    if (!this.options.actorLabel) throw new Error("actorLabel is required");
  }

  async execute(): Promise<TaskResult> {
    const code = withResolvedActor(this.options.actorLabel, [
      `_sa = _actor.get_external_save_asset()`,
      `print("VOXEL_SAVE_ASSET=" + (_sa.get_path_name() if _sa is not None else "None"))`,
    ]);
    return this.call("editor.execute_python", { code });
  }
}
