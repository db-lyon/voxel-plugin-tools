import { SpawnVoxelActor } from "./SpawnVoxelActor.js";

/**
 * Drop an `AVoxelHeightSculptActor` into the active level — the target
 * actor for 2D/height sculpting (paint, flatten, smooth, sculpt-height).
 *
 * Header: `Voxel/Public/Sculpt/Height/VoxelHeightSculptActor.h`. Class path
 * `/Script/Voxel.VoxelHeightSculptActor`. Wraps one host call: `level.place_actor`.
 */
export default class SpawnHeightSculptActor extends SpawnVoxelActor {
  get taskName(): string { return "voxel.spawn_height_sculpt_actor"; }
  protected readonly actorClass = "/Script/Voxel.VoxelHeightSculptActor";
}
