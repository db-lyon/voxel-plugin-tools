import { SpawnVoxelActor } from "./SpawnVoxelActor.js";

/**
 * Drop an `AVoxelVolumeSculptActor` into the active level — the target
 * actor for 3D/volume sculpting (sphere, cube, surface, angle, flatten, smooth).
 *
 * Header: `Voxel/Public/Sculpt/Volume/VoxelVolumeSculptActor.h`. Class path
 * `/Script/Voxel.VoxelVolumeSculptActor`. Wraps one host call: `level.place_actor`.
 */
export default class SpawnVolumeSculptActor extends SpawnVoxelActor {
  get taskName(): string { return "voxel.spawn_volume_sculpt_actor"; }
  protected readonly actorClass = "/Script/Voxel.VoxelVolumeSculptActor";
}
