import { BaseTask, type TaskResult } from "@db-lyon/flowkit";

interface MeshEntry {
  mesh: string;
  weight?: number;
}

interface Options {
  assetPath: string;
  meshes: MeshEntry[];
  pointsPerSquaredMeter?: number;
  seed?: number;
}

/**
 * Build a PCG graph that scatters weighted static meshes on a voxel terrain.
 *
 * Wraps `UPCGVoxelSamplerSettings` (VoxelPCG/Public/PCGVoxelSampler.h) feeding
 * `UPCGStaticMeshSpawnerSettings`. The output asset is a UPCGGraph that the
 * caller attaches to a PCG component (typically on or near an AVoxelWorld)
 * before calling `pcg.execute`.
 *
 * Layer wiring (FVoxelStackLayer struct) is intentionally not exposed in v1.
 * The sampler defaults to the UE-side default stack/layer, which is what an
 * out-of-the-box voxel world uses.
 */
export default class BuildScatterGraph extends BaseTask<Options> {
  get taskName(): string { return "voxel.build_scatter_graph"; }

  protected validate(): void {
    if (!this.options.assetPath) throw new Error("assetPath is required");
    if (!Array.isArray(this.options.meshes) || this.options.meshes.length === 0) {
      throw new Error("meshes must be a non-empty array of { mesh, weight? } entries");
    }
    for (const m of this.options.meshes) {
      if (!m?.mesh) throw new Error("each meshes[] entry must include a `mesh` asset path");
    }
  }

  async execute(): Promise<TaskResult> {
    const { assetPath, meshes, pointsPerSquaredMeter = 0.1, seed = 1 } = this.options;

    const slash = assetPath.lastIndexOf("/");
    if (slash <= 0) {
      return { success: false, error: new Error(`assetPath must be a content path like /Game/Folder/Name, got: ${assetPath}`) };
    }
    const packagePath = assetPath.slice(0, slash);
    const name = assetPath.slice(slash + 1);

    const create = await this.call("pcg.create_graph", { name, packagePath });
    if (!create.success) {
      // Re-running over an existing graph is a supported workflow; only abort
      // on hard failures, not on the "asset already exists" path.
      const msg = create.error?.message ?? "";
      if (!/exist|already/i.test(msg)) return create;
    }

    // Bridge's nodeType resolver only falls back to /Script/PCG.* — for
    // VoxelPCG classes we pass the absolute object path so the first
    // FindObject lookup hits.
    const samplerR = await this.call("pcg.add_node", {
      assetPath,
      nodeType: "/Script/VoxelPCG.PCGVoxelSamplerSettings",
    });
    if (!samplerR.success) return samplerR;
    const samplerName = samplerR.data?.nodeName as string | undefined;
    if (!samplerName) return { success: false, error: new Error("pcg.add_node sampler returned no nodeName") };

    const spawnerR = await this.call("pcg.add_node", {
      assetPath,
      nodeType: "/Script/PCG.PCGStaticMeshSpawnerSettings",
    });
    if (!spawnerR.success) return spawnerR;
    const spawnerName = spawnerR.data?.nodeName as string | undefined;
    if (!spawnerName) return { success: false, error: new Error("pcg.add_node spawner returned no nodeName") };

    const settingsR = await this.call("pcg.set_node_settings", {
      assetPath,
      nodeName: samplerName,
      settings: {
        PointsPerSquaredMeter: pointsPerSquaredMeter,
        Seed: seed,
      },
    });
    if (!settingsR.success) return settingsR;

    const connectR = await this.call("pcg.connect_nodes", {
      assetPath,
      sourceNode: samplerName,
      targetNode: spawnerName,
    });
    if (!connectR.success) return connectR;

    const meshesR = await this.call("pcg.set_static_mesh_spawner_meshes", {
      assetPath,
      nodeName: spawnerName,
      entries: meshes,
      replace: true,
    });
    if (!meshesR.success) return meshesR;

    return {
      success: true,
      data: {
        assetPath,
        samplerNode: samplerName,
        spawnerNode: spawnerName,
        meshCount: meshes.length,
        pointsPerSquaredMeter,
        seed,
      },
    };
  }
}
