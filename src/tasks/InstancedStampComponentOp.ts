import { UeMcpTask, type TaskResult } from "ue-mcp/task";
import { withResolvedActor, pyStr } from "./_python.js";

export interface InstancedCompOptions {
  actorLabel: string;
  /** Name of the UVoxelInstancedStampComponent on the actor. */
  componentName: string;
}

/**
 * Base for management ops on a UVoxelInstancedStampComponent. Resolves the named
 * component on the actor into `_comp`, then runs the subclass body. Wraps one
 * `editor.execute_python` call.
 */
export abstract class InstancedStampComponentOp<TOpts extends InstancedCompOptions> extends UeMcpTask<TOpts> {
  /** Python statements operating on the resolved `_comp`. */
  protected abstract bodyLines(): string[];

  protected validate(): void {
    if (!this.options.actorLabel) throw new Error("actorLabel is required");
    if (!this.options.componentName) throw new Error("componentName is required");
  }

  async execute(): Promise<TaskResult> {
    const name = pyStr(this.options.componentName);
    const code = withResolvedActor(this.options.actorLabel, [
      `_comp = next((c for c in _actor.get_components_by_class(unreal.VoxelInstancedStampComponent) if c.get_name() == ${name}), None)`,
      `if _comp is None:`,
      `    raise Exception("instanced stamp component not found: " + ${name})`,
      ...this.bodyLines(),
    ]);
    return this.call("editor.execute_python", { code });
  }
}
