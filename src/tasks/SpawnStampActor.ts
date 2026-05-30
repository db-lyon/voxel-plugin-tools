import { SpawnVoxelActor } from "./SpawnVoxelActor.js";

/**
 * Atomic. Drop an `AVoxelStampActor` into the active level — a placement actor
 * that owns a single stamp (sculpt/displacement applied to nearby voxel worlds).
 *
 * Header: `Voxel/Public/VoxelStampActor.h` (`AVoxelStampActor`). Class path
 * `/Script/Voxel.VoxelStampActor`. Wraps exactly one host call: `level.place_actor`.
 */
export default class SpawnStampActor extends SpawnVoxelActor {
  get taskName(): string { return "voxel.spawn_stamp_actor"; }
  protected readonly actorClass = "/Script/Voxel.VoxelStampActor";
}
