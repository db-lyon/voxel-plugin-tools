import { UeMcpTask, type TaskResult } from "ue-mcp/task";
import { withResolvedActor } from "./_python.js";

export interface VolumeSculptOptions {
  /** Label of the target `AVoxelVolumeSculptActor` in the active editor world. */
  actorLabel: string;
}

/**
 * Base for atomic "run one volume-sculpt operation" tasks. Each subclass bakes
 * one function on `VoxelVolumeSculptBlueprintLibrary_BlueprintOnly` and supplies
 * its already-marshalled Python argument expressions (the leading `_actor` is
 * prepended here). The body wraps exactly one host call — `editor.execute_python`
 * — because these are static library functions, not actor/component methods.
 *
 * Header: `Voxel/Public/Sculpt/Volume/VoxelVolumeSculptBlueprintLibrary.h`
 * (+ the UHT `_BlueprintOnly` wrapper).
 */
export abstract class VolumeSculptOp<TOpts extends VolumeSculptOptions> extends UeMcpTask<TOpts> {
  /** snake_case function name on `VoxelVolumeSculptBlueprintLibrary_BlueprintOnly`. */
  protected abstract readonly functionName: string;

  /** Python arg expressions AFTER the `_actor` positional (centre, kwargs, …). */
  protected abstract callArgs(): string[];

  protected validate(): void {
    if (!this.options.actorLabel) throw new Error("actorLabel is required");
  }

  async execute(): Promise<TaskResult> {
    const args = ["_actor", ...this.callArgs()].join(", ");
    const code = withResolvedActor(this.options.actorLabel, [
      `unreal.VoxelVolumeSculptBlueprintLibrary_BlueprintOnly.${this.functionName}(${args})`,
      `print(${JSON.stringify(`voxel.${this.functionName}: applied to `)} + _actor.get_actor_label())`,
    ]);
    return this.call("editor.execute_python", { code });
  }
}
