import { UeMcpTask, type TaskResult } from "ue-mcp/task";

/** Friendly node key -> VoxelPCG UPCGSettings class (under /Script/VoxelPCG). */
const NODE_CLASS: Record<string, string> = {
  call_graph: "PCGCallVoxelGraphSettings",
  sampler: "PCGVoxelSamplerSettings",
  sampler_v2: "PCGVoxelSamplerV2Settings",
  query: "PCGVoxelQuerySettings",
  stamp_spawner: "PCGVoxelStampSpawnerSettings",
  spawn_actor: "PCGSpawnActorWithVoxelGraphSettings",
  apply_on_graph: "PCGApplyOnVoxelGraphSettings",
  create_spline: "PCGCreateVoxelSplineSettings",
  projection: "PCGVoxelProjectionSettings",
  elevation_isolines: "PCGVoxelElevationIsolines",
  wait_for_world: "PCGWaitForVoxelWorldSettings",
  layer_sampler: "PCGVoxelLayerSamplerSettings",
  landscape_sampler: "PCGVoxelLandscapeSamplerSettings",
  landscape_projection: "PCGVoxelLandscapeProjectionSettings",
  wait_for_landscape: "PCGWaitForVoxelLandscapeSettings",
};

interface Options {
  /** Object path to the target UPCGGraph. */
  assetPath: string;
  /** Which VoxelPCG node to add (see NODE_CLASS keys). */
  node: string;
  nodeName?: string;
}

/**
 * Add a VoxelPCG node to a PCG graph (e.g. the voxel-surface sampler, or a
 * "call voxel graph" node). Wraps the host `pcg.add_node` with the
 * `/Script/VoxelPCG.<Class>` baked from a friendly `node` key. Configure the
 * added node afterwards with the host `pcg.set_node_settings`.
 *
 * Module: VoxelPCG (UPCGSettings subclasses).
 */
export default class PcgAddNode extends UeMcpTask<Options> {
  get taskName(): string { return "voxel.pcg_add_node"; }

  protected validate(): void {
    if (!this.options.assetPath) throw new Error("assetPath (PCG graph) is required");
    if (!this.options.node || !NODE_CLASS[this.options.node]) {
      throw new Error(`node must be one of ${Object.keys(NODE_CLASS).join("|")}`);
    }
  }

  async execute(): Promise<TaskResult> {
    const params: Record<string, unknown> = {
      assetPath: this.options.assetPath,
      nodeType: `/Script/VoxelPCG.${NODE_CLASS[this.options.node]}`,
    };
    if (this.options.nodeName !== undefined) params.nodeName = this.options.nodeName;
    return this.call("pcg.add_node", params);
  }
}
