import { UeMcpTask, type TaskResult } from "ue-mcp/task";

export interface Vec3 { x: number; y: number; z: number; }

export interface SpawnOptions {
  label?: string;
  location?: Vec3;
  rotation?: Vec3;
  scale?: Vec3;
}

/**
 * Base for atomic "spawn a Voxel actor" tasks. Each concrete subclass bakes
 * exactly one `/Script/<Module>.<Class>` path; the body wraps exactly one
 * host call — `level.place_actor` — with that class baked in.
 *
 * Property setup (LayerStack, MegaMaterial, VoxelSize, …) is a separate
 * `level.set_actor_property` call — see the `voxel.set_world_*` tasks.
 */
export abstract class SpawnVoxelActor extends UeMcpTask<SpawnOptions> {
  /** The `/Script/<Module>.<Class>` path this task spawns. */
  protected abstract readonly actorClass: string;

  async execute(): Promise<TaskResult> {
    const { label, location, rotation, scale } = this.options;
    const params: Record<string, unknown> = { actorClass: this.actorClass };
    if (label !== undefined) params.label = label;
    if (location !== undefined) params.location = location;
    if (rotation !== undefined) params.rotation = rotation;
    if (scale !== undefined) params.scale = scale;
    return this.call("level.place_actor", params);
  }
}
