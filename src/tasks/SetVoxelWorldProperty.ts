import { UeMcpTask, type TaskResult } from "ue-mcp/task";

export interface SetVoxelWorldPropertyOptions {
  actorLabel: string;
  value: unknown;
}

/**
 * Base for "set one `AVoxelWorld` UPROPERTY" tasks. Each subclass bakes
 * exactly one property name; the body wraps exactly one host call —
 * `level.set_actor_property`. Header for all: `Voxel/Public/VoxelWorld.h`.
 */
export abstract class SetVoxelWorldProperty extends UeMcpTask<SetVoxelWorldPropertyOptions> {
  /** The `AVoxelWorld` UPROPERTY this task writes. */
  protected abstract readonly propertyName: string;

  protected validate(): void {
    if (!this.options.actorLabel) throw new Error("actorLabel is required");
    if (this.options.value === undefined) throw new Error("value is required");
  }

  async execute(): Promise<TaskResult> {
    const { actorLabel, value } = this.options;
    return this.call("level.set_actor_property", {
      actorLabel,
      propertyName: this.propertyName,
      value,
    });
  }
}
