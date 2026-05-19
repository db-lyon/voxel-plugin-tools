import { BaseTask, type TaskResult } from "@db-lyon/flowkit";
import { addNode, connectNodes, setNodeSettings } from "../shared/voxelGraph.js";

interface Options {
  graphPath: string;
  stampAsset: string;
  count?: number;
  radius?: number;
  seed?: number;
}

/**
 * Builds a PCG graph that places voxel stamp assets across a terrain. Pipeline:
 * VoxelSampler → SurfaceSampler → DensityFilter (thinning by `count`) →
 * VoxelStamp.
 *
 * `count` is a soft cap - the density filter approximates it from the sampled
 * point cloud, so the final placement count varies with the underlying voxel
 * volume density.
 */
export default class SpawnStamps extends BaseTask<Options> {
  get taskName(): string { return "voxel.spawn_stamps"; }

  protected validate(): void {
    if (!this.options.graphPath) throw new Error("graphPath is required");
    if (!this.options.stampAsset) throw new Error("stampAsset is required");
  }

  async execute(): Promise<TaskResult> {
    const { graphPath, stampAsset, count = 50, radius = 500, seed = 1 } = this.options;

    await this.call("pcg.create_graph", { path: graphPath });

    const sampler  = await addNode(this, graphPath, "voxelSampler");
    const surface  = await addNode(this, graphPath, "surfaceSampler");
    const density_ = await addNode(this, graphPath, "densityFilter");
    const stamp    = await addNode(this, graphPath, "voxelStamp");

    // PointsPerSquaredMeter is approximated from count/radius² to give the
    // surface sampler a sensible density for the desired stamp count.
    const pointsPerSqM = Math.max(0.0001, count / (Math.PI * radius * radius));

    await setNodeSettings(this, graphPath, surface, {
      PointsPerSquaredMeter: pointsPerSqM,
      Radius: radius,
      Seed: seed,
    });
    await setNodeSettings(this, graphPath, density_, { LowerBound: 0.5, UpperBound: 1.0, Seed: seed });
    await setNodeSettings(this, graphPath, stamp,    { StampAsset: stampAsset });

    await connectNodes(this, graphPath, sampler,  surface);
    await connectNodes(this, graphPath, surface,  density_);
    await connectNodes(this, graphPath, density_, stamp);

    return {
      success: true,
      data: {
        graphPath,
        nodes: { sampler, surface, density: density_, stamp },
        stampAsset,
        countEstimate: count,
      },
    };
  }
}
