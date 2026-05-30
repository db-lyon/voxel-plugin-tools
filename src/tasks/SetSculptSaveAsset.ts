import { UeMcpTask, type TaskResult } from "ue-mcp/task";
import { withResolvedActor, pyStr } from "./_python.js";

interface Options {
  /** Label of a height or volume sculpt actor. */
  actorLabel: string;
  /** Object path to a matching UVoxelHeightSculptSaveAsset / UVoxelVolumeSculptSaveAsset. */
  savePath: string;
}

/**
 * Link a sculpt actor to an external save asset so its edits persist to disk
 * (height -> UVoxelHeightSculptSaveAsset, volume -> UVoxelVolumeSculptSaveAsset;
 * the asset type must match the actor). Wraps editor.execute_python — the setter
 * takes a UObject asset arg, which the host invoke_function can't pass.
 *
 * Header: `Voxel/Public/Sculpt/...SculptActor.h` (`SetExternalSaveAsset`).
 */
export default class SetSculptSaveAsset extends UeMcpTask<Options> {
  get taskName(): string { return "voxel.set_sculpt_save_asset"; }

  protected validate(): void {
    if (!this.options.actorLabel) throw new Error("actorLabel is required");
    if (!this.options.savePath) throw new Error("savePath is required");
  }

  async execute(): Promise<TaskResult> {
    const p = pyStr(this.options.savePath);
    const code = withResolvedActor(this.options.actorLabel, [
      `_sa = unreal.load_asset(${p})`,
      `if _sa is None:`,
      `    raise Exception("save asset not found: " + ${p})`,
      `_actor.set_external_save_asset(_sa)`,
      `print("voxel.set_sculpt_save_asset: linked " + _actor.get_actor_label() + " -> " + ${p})`,
    ]);
    return this.call("editor.execute_python", { code });
  }
}
