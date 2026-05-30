import { SpawnVoxelActor } from "./SpawnVoxelActor.js";

/**
 * Atomic. Drop an `AVoxelWorld` actor into the active level.
 *
 * Header: `Voxel/Public/VoxelWorld.h` (`AVoxelWorld`). Class path
 * `/Script/Voxel.VoxelWorld`. Wraps exactly one host call: `level.place_actor`.
 */
export default class SpawnWorld extends SpawnVoxelActor {
  get taskName(): string { return "voxel.spawn_world"; }
  protected readonly actorClass = "/Script/Voxel.VoxelWorld";
}
