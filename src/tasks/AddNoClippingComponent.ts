import { UeMcpTask, type TaskResult } from "ue-mcp/task";

interface Options {
  actorLabel: string;
  componentName?: string;
}

/**
 * Attach a UVoxelNoClippingComponent to an actor — keeps the owner from clipping
 * through voxel terrain/overhangs (auto-teleports above solid ground). Configure
 * its layer with voxel_set_no_clipping_layer. Wraps level.add_component.
 * Header: Voxel/Public/VoxelNoClippingComponent.h.
 */
export default class AddNoClippingComponent extends UeMcpTask<Options> {
  get taskName(): string { return "voxel.add_no_clipping_component"; }

  protected validate(): void {
    if (!this.options.actorLabel) throw new Error("actorLabel is required");
  }

  async execute(): Promise<TaskResult> {
    const params: Record<string, unknown> = {
      actorLabel: this.options.actorLabel,
      componentClass: "/Script/Voxel.VoxelNoClippingComponent",
    };
    if (this.options.componentName !== undefined) params.componentName = this.options.componentName;
    return this.call("level.add_component", params);
  }
}
