import { SetVoxelWorldProperty } from "./SetVoxelWorldProperty.js";

/**
 * Set `AVoxelWorld::bEnableNavigation` — navmesh generation over the
 * voxel surface (default true; required for AI pathfinding). `value` is boolean.
 *
 * Header: `Voxel/Public/VoxelWorld.h` (`bool bEnableNavigation`).
 * Wraps one host call: `level.set_actor_property`.
 */
export default class SetEnableNavigation extends SetVoxelWorldProperty {
  get taskName(): string { return "voxel.set_world_enable_navigation"; }
  protected readonly propertyName = "bEnableNavigation";
}
