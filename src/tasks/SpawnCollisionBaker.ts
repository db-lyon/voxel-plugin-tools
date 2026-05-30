import { SpawnVoxelActor } from "./SpawnVoxelActor.js";

/**
 * Atomic. Drop an `AVoxelCollisionBaker` into the active level — bakes collision
 * geometry for a voxel layer within a radius (set `bGenerate` to trigger).
 *
 * Header: `Voxel/Public/Collision/VoxelCollisionBaker.h`. Class path
 * `/Script/Voxel.VoxelCollisionBaker`. Wraps one host call: `level.place_actor`.
 */
export default class SpawnCollisionBaker extends SpawnVoxelActor {
  get taskName(): string { return "voxel.spawn_collision_baker"; }
  protected readonly actorClass = "/Script/Voxel.VoxelCollisionBaker";
}
