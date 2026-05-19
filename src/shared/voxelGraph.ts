import type { TaskResult } from "@db-lyon/flowkit";

/**
 * Cross-task helpers for building Voxel-Plugin-flavoured PCG graphs. None of
 * these are referenced by ue-mcp.plugin.yml directly - task files import them.
 *
 * The exact PCG node-type names depend on the Voxel Plugin version installed.
 * They are intentionally string constants so they can be updated in one
 * place when the plugin's exposed nodes shift.
 */

export const VOXEL_NODES = {
  /** Samples the active voxel volume in PCG space. */
  voxelSampler: "VoxelSampler",
  /** Surface sampler that filters points to voxel-terrain surface positions. */
  surfaceSampler: "PCGSurfaceSampler",
  /** Static-mesh spawner consuming the points from the surface sampler. */
  staticMeshSpawner: "PCGStaticMeshSpawner",
  /** Stamp-spawner specific to Voxel Plugin's stamp asset format. */
  voxelStamp: "VoxelStamp",
  /** Density filter for thinning out sampled points. */
  densityFilter: "PCGDensityFilter",
  /** Transform-randomiser for scale/rotation jitter. */
  transformPoints: "PCGTransformPoints",
} as const;

export type VoxelNodeKey = keyof typeof VOXEL_NODES;

/**
 * Tasks pass `this` in; `BaseTask.call` is `protected` so the helper accepts
 * `unknown` and casts internally. The cast is hidden here so task files stay
 * clean.
 */
interface CallSurface {
  call(taskName: string, options?: Record<string, unknown>): Promise<TaskResult>;
}

function callable(task: unknown): CallSurface {
  return task as CallSurface;
}

/**
 * Type-safe wrapper around `this.call('pcg.add_node', ...)`. Returns the
 * caller-visible node id reported by the bridge, or throws so the task gets
 * a meaningful error.
 */
export async function addNode(
  task: unknown,
  graphPath: string,
  nodeKey: VoxelNodeKey,
): Promise<string> {
  const r = await callable(task).call("pcg.add_node", {
    graphPath,
    nodeType: VOXEL_NODES[nodeKey],
  });
  if (!r.success) {
    throw new Error(`pcg.add_node ${nodeKey} failed: ${r.error?.message ?? "unknown"}`);
  }
  const id = (r.data?.nodeId ?? r.data?.id) as string | undefined;
  if (!id) {
    throw new Error(`pcg.add_node ${nodeKey} did not return a node id`);
  }
  return id;
}

export async function connectNodes(
  task: unknown,
  graphPath: string,
  sourceId: string,
  targetId: string,
): Promise<void> {
  const r = await callable(task).call("pcg.connect_nodes", {
    graphPath,
    sourceNodeId: sourceId,
    targetNodeId: targetId,
  });
  if (!r.success) {
    throw new Error(`pcg.connect_nodes failed: ${r.error?.message ?? "unknown"}`);
  }
}

export async function setNodeSettings(
  task: unknown,
  graphPath: string,
  nodeId: string,
  settings: Record<string, unknown>,
): Promise<void> {
  const r = await callable(task).call("pcg.set_node_settings", {
    graphPath,
    nodeId,
    settings,
  });
  if (!r.success) {
    throw new Error(`pcg.set_node_settings failed: ${r.error?.message ?? "unknown"}`);
  }
}
