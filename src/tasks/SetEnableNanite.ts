import { SetVoxelWorldProperty } from "./SetVoxelWorldProperty.js";

/**
 * Atomic. Set `AVoxelWorld::bEnableNanite` — Nanite rendering of voxel meshes
 * (default true). `value` is a boolean.
 *
 * Header: `Voxel/Public/VoxelWorld.h` (`bool bEnableNanite`).
 * Wraps one host call: `level.set_actor_property`.
 */
export default class SetEnableNanite extends SetVoxelWorldProperty {
  get taskName(): string { return "voxel.set_world_enable_nanite"; }
  protected readonly propertyName = "bEnableNanite";
}
