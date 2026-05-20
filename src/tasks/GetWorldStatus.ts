import { BaseTask, type TaskResult } from "@db-lyon/flowkit";

interface Options {
  actorLabel: string;
  world?: "editor" | "pie";
}

interface InvokeData {
  returnValues?: Record<string, string>;
}

/**
 * One-call snapshot of an `AVoxelWorld`'s lifecycle state.
 *
 * Composes 5x `editor.invoke_function` against the zero-arg lifecycle
 * UFUNCTIONs on `AVoxelWorld` (`Voxel/Public/VoxelWorld.h`):
 *
 *   bool  IsRuntimeCreated()
 *   bool  IsVoxelWorldReady()
 *   bool  IsProcessingNewState()
 *   float GetProgress()
 *   int32 GetNumPendingTasks()
 *
 * Use it before any operation that requires the voxel runtime to be live —
 * scattering meshes / placing stamps into a half-built world produces empty
 * or stale output. The five calls fan out in parallel.
 */
export default class GetWorldStatus extends BaseTask<Options> {
  get taskName(): string { return "voxel.get_world_status"; }

  protected validate(): void {
    if (!this.options.actorLabel) throw new Error("actorLabel is required");
  }

  async execute(): Promise<TaskResult> {
    const { actorLabel, world } = this.options;
    const baseParams: Record<string, unknown> = { actorLabel };
    if (world) baseParams.world = world;

    const fns = [
      "IsRuntimeCreated",
      "IsVoxelWorldReady",
      "IsProcessingNewState",
      "GetProgress",
      "GetNumPendingTasks",
    ] as const;

    const results = await Promise.all(
      fns.map(functionName =>
        this.call("editor.invoke_function", { ...baseParams, functionName })
      )
    );

    for (let i = 0; i < results.length; i++) {
      if (!results[i].success) return results[i];
    }

    const ret = (r: TaskResult): string | undefined =>
      (r.data as InvokeData | undefined)?.returnValues?.ReturnValue;
    const asBool = (s: string | undefined): boolean => s === "true" || s === "True";
    const asNum = (s: string | undefined): number => (s == null ? NaN : Number(s));

    return {
      success: true,
      data: {
        actorLabel,
        isRuntimeCreated:     asBool(ret(results[0])),
        isVoxelWorldReady:    asBool(ret(results[1])),
        isProcessingNewState: asBool(ret(results[2])),
        progress:             asNum(ret(results[3])),
        numPendingTasks:      asNum(ret(results[4])),
      },
    };
  }
}
