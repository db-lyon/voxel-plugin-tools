import { BaseTask, type TaskResult } from "@db-lyon/flowkit";

interface Vec3 { x: number; y: number; z: number; }

interface Options {
  label?: string;
  location?: Vec3;
  rotation?: Vec3;
  scale?: Vec3;
}

/**
 * Atomic. Drop an `AVoxelWorld` actor into the active level.
 *
 * Wraps exactly one host call: `level.place_actor` with the Voxel-Plugin
 * class path baked in. Property setup (LayerStack, MegaMaterial, etc.)
 * is the caller's job — use `level.set_actor_property`, or run the
 * `voxel_hello_world` flow which chains those calls with sane defaults.
 *
 * Header: `Voxel/Public/VoxelWorld.h` (`AVoxelWorld`).
 */
export default class SpawnWorld extends BaseTask<Options> {
  get taskName(): string { return "voxel.spawn_world"; }

  async execute(): Promise<TaskResult> {
    const { label, location, rotation, scale } = this.options;
    const params: Record<string, unknown> = {
      actorClass: "/Script/Voxel.VoxelWorld",
    };
    if (label !== undefined) params.label = label;
    if (location !== undefined) params.location = location;
    if (rotation !== undefined) params.rotation = rotation;
    if (scale !== undefined) params.scale = scale;
    return this.call("level.place_actor", params);
  }
}
