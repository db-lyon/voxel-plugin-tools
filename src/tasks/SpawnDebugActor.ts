import { SpawnVoxelActor } from "./SpawnVoxelActor.js";

/**
 * Drop an `AVoxelDebugActor` into the active level — visualizes a voxel
 * layer over a debug region (LOD, size, grayscale/color settings). Dev tool.
 *
 * Header: `Voxel/Public/VoxelDebugActor.h`. Class path
 * `/Script/Voxel.VoxelDebugActor`. Wraps one host call: `level.place_actor`.
 */
export default class SpawnDebugActor extends SpawnVoxelActor {
  get taskName(): string { return "voxel.spawn_debug_actor"; }
  protected readonly actorClass = "/Script/Voxel.VoxelDebugActor";
}
