import { SetVoxelWorldProperty } from "./SetVoxelWorldProperty.js";

/**
 * Atomic. Set `AVoxelWorld::VoxelSize` — edge length of one voxel in cm
 * (default 100). Lower = finer detail + heavier cost. `value` is an integer.
 *
 * Header: `Voxel/Public/VoxelWorld.h` (`int32 VoxelSize`).
 * Wraps one host call: `level.set_actor_property`.
 */
export default class SetVoxelSize extends SetVoxelWorldProperty {
  get taskName(): string { return "voxel.set_world_voxel_size"; }
  protected readonly propertyName = "VoxelSize";
}
