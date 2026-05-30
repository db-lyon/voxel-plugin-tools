import { UeMcpTask, type TaskResult } from "ue-mcp/task";

export interface VoxelWorldFunctionOptions {
  actorLabel: string;
  world?: "editor" | "pie";
}

/**
 * Base for atomic "invoke a no-arg `AVoxelWorld` UFUNCTION" tasks. Each
 * subclass bakes exactly one function name; the body wraps exactly one host
 * call — `editor.invoke_function`.
 *
 * For functions that return a value, the result lands at
 * `data.returnValues.ReturnValue` (a string) — parse on the caller side.
 * Header for all: `Voxel/Public/VoxelWorld.h`.
 */
export abstract class InvokeVoxelWorldFunction extends UeMcpTask<VoxelWorldFunctionOptions> {
  /** The `AVoxelWorld` UFUNCTION to invoke (must take no arguments). */
  protected abstract readonly functionName: string;

  protected validate(): void {
    if (!this.options.actorLabel) throw new Error("actorLabel is required");
  }

  async execute(): Promise<TaskResult> {
    const { actorLabel, world } = this.options;
    const params: Record<string, unknown> = {
      actorLabel,
      functionName: this.functionName,
    };
    if (world) params.world = world;
    return this.call("editor.invoke_function", params);
  }
}
