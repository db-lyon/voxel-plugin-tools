import { BaseTask, type TaskResult } from "@db-lyon/flowkit";
import { addNode, connectNodes, setNodeSettings } from "../shared/voxelGraph.js";

interface Options {
  graphPath: string;
  mesh: string;
  density?: number;
  minScale?: number;
  maxScale?: number;
  seed?: number;
}

/**
 * Builds a PCG graph that scatters static meshes on a Voxel Plugin Pro
 * terrain. Pipeline: VoxelSampler → SurfaceSampler → DensityFilter →
 * TransformPoints → StaticMeshSpawner.
 *
 * Idempotency: if the graph already exists, the task assumes the caller
 * wants a fresh build and will append nodes. Callers expecting strict
 * idempotency should delete the graph first via `pcg(action="delete_graph")`
 * (or skip the call entirely when no rebuild is needed).
 */
export default class ScatterOnTerrain extends BaseTask<Options> {
  get taskName(): string { return "vpp.scatter_on_terrain"; }

  protected validate(): void {
    if (!this.options.graphPath) throw new Error("graphPath is required");
    if (!this.options.mesh) throw new Error("mesh is required");
  }

  async execute(): Promise<TaskResult> {
    const { graphPath, mesh, density = 0.25, minScale = 0.8, maxScale = 1.4, seed = 1 } = this.options;

    // Create the graph (no-op if pcg.create_graph rejects an existing one;
    // we proceed regardless so add_node can land on a pre-existing graph).
    await this.call("pcg.create_graph", { path: graphPath });

    const sampler   = await addNode(this, graphPath, "voxelSampler");
    const surface   = await addNode(this, graphPath, "surfaceSampler");
    const density_  = await addNode(this, graphPath, "densityFilter");
    const transform = await addNode(this, graphPath, "transformPoints");
    const spawner   = await addNode(this, graphPath, "staticMeshSpawner");

    await setNodeSettings(this, graphPath, surface,   { PointsPerSquaredMeter: density });
    await setNodeSettings(this, graphPath, density_,  { LowerBound: 0.0, UpperBound: 1.0, Seed: seed });
    await setNodeSettings(this, graphPath, transform, { MinScale: minScale, MaxScale: maxScale, Seed: seed });
    await setNodeSettings(this, graphPath, spawner,   { StaticMesh: mesh });

    await connectNodes(this, graphPath, sampler,   surface);
    await connectNodes(this, graphPath, surface,   density_);
    await connectNodes(this, graphPath, density_,  transform);
    await connectNodes(this, graphPath, transform, spawner);

    return {
      success: true,
      data: {
        graphPath,
        nodes: { sampler, surface, density: density_, transform, spawner },
        mesh,
      },
      // No automatic rollback - PCG node creation/connection has no inverse
      // exposed by the bridge. Callers wanting safety should run inside a
      // flow with git_snapshot enabled.
    };
  }
}
