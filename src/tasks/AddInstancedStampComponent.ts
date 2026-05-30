import { UeMcpTask, type TaskResult } from "ue-mcp/task";

interface Options {
  actorLabel: string;
  componentName?: string;
}

/**
 * Attach a UVoxelInstancedStampComponent to an actor — holds many stamps in one
 * component (scatter rocks / craters / heightmap patches without spawning an
 * actor each). Wraps level.add_component. Header: Voxel/Public/VoxelInstancedStampComponent.h.
 */
export default class AddInstancedStampComponent extends UeMcpTask<Options> {
  get taskName(): string { return "voxel.add_instanced_stamp_component"; }

  protected validate(): void {
    if (!this.options.actorLabel) throw new Error("actorLabel is required");
  }

  async execute(): Promise<TaskResult> {
    const params: Record<string, unknown> = {
      actorLabel: this.options.actorLabel,
      componentClass: "/Script/Voxel.VoxelInstancedStampComponent",
    };
    if (this.options.componentName !== undefined) params.componentName = this.options.componentName;
    return this.call("level.add_component", params);
  }
}
