import { UeMcpTask, type TaskResult } from "ue-mcp/task";
import { withResolvedActor } from "./_python.js";

export interface SculptActorOptions {
  /** Label of the target sculpt actor in the active editor world. */
  actorLabel: string;
}

/**
 * Base for "run one sculpt-library operation" tasks. Voxel's sculpt ops
 * are static functions on UHT-generated `*_BlueprintOnly` libraries — the host
 * `editor.invoke_function` only targets actor/component instances, so each op
 * wraps exactly one `editor.execute_python` call. Subclasses bake the library
 * class + function name and supply the marshalled Python args (the leading
 * `_actor` positional is prepended here).
 */
export abstract class SculptOp<TOpts extends SculptActorOptions> extends UeMcpTask<TOpts> {
  /** UHT `_BlueprintOnly` library class, e.g. `VoxelVolumeSculptBlueprintLibrary_BlueprintOnly`. */
  protected abstract readonly libraryClass: string;
  /** snake_case function name on `libraryClass`. */
  protected abstract readonly functionName: string;
  /** Python arg expressions AFTER the `_actor` positional (centre, kwargs, …). */
  protected abstract callArgs(): string[];

  /** Python statements to run before the call (e.g. build a graph wrapper into `_graph`). */
  protected setupLines(): string[] { return []; }

  protected validate(): void {
    if (!this.options.actorLabel) throw new Error("actorLabel is required");
  }

  async execute(): Promise<TaskResult> {
    const args = ["_actor", ...this.callArgs()].join(", ");
    const code = withResolvedActor(this.options.actorLabel, [
      ...this.setupLines(),
      `unreal.${this.libraryClass}.${this.functionName}(${args})`,
      `print(${JSON.stringify(`voxel.${this.functionName}: applied to `)} + _actor.get_actor_label())`,
    ]);
    return this.call("editor.execute_python", { code });
  }
}
