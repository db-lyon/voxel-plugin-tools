import { UeMcpTask, type TaskResult } from "ue-mcp/task";

interface Options {
  actorLabel: string;
  world?: "editor" | "pie";
}

/**
 * Atomic. Read `AVoxelWorld::IsVoxelWorldReady()` on a named voxel-world
 * actor. The single most useful signal for "is it safe to scatter /
 * stamp / sample now?".
 *
 * Wraps exactly one host call: `editor.invoke_function`. The return
 * value is passed through unchanged in `data.returnValues.ReturnValue`
 * as the string `"true"` or `"false"` — parse on the caller side.
 *
 * Header: `Voxel/Public/VoxelWorld.h` (`bool IsVoxelWorldReady()`).
 */
export default class IsWorldReady extends UeMcpTask<Options> {
  get taskName(): string { return "voxel.is_world_ready"; }

  protected validate(): void {
    if (!this.options.actorLabel) throw new Error("actorLabel is required");
  }

  async execute(): Promise<TaskResult> {
    const { actorLabel, world } = this.options;
    const params: Record<string, unknown> = {
      actorLabel,
      functionName: "IsVoxelWorldReady",
    };
    if (world) params.world = world;
    return this.call("editor.invoke_function", params);
  }
}
