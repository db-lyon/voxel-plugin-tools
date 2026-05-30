import { UeMcpTask, type TaskResult } from "ue-mcp/task";

interface Options {
  actorLabel: string;
  componentName?: string;
}

/**
 * Atomic. Attach a `UVoxelStampComponent` to an existing actor — lets any actor
 * carry a voxel stamp (sculpt/displacement applied to nearby voxel worlds).
 * Wraps exactly one host call: `level.add_component`.
 *
 * Header: `Voxel/Public/VoxelStampComponent.h`. Class path
 * `/Script/Voxel.VoxelStampComponent`.
 */
export default class AddStampComponent extends UeMcpTask<Options> {
  get taskName(): string { return "voxel.add_stamp_component"; }

  protected validate(): void {
    if (!this.options.actorLabel) throw new Error("actorLabel is required");
  }

  async execute(): Promise<TaskResult> {
    const params: Record<string, unknown> = {
      actorLabel: this.options.actorLabel,
      componentClass: "/Script/Voxel.VoxelStampComponent",
    };
    if (this.options.componentName !== undefined) {
      params.componentName = this.options.componentName;
    }
    return this.call("level.add_component", params);
  }
}
