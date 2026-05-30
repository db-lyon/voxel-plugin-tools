import { SetVoxelWorldProperty } from "./SetVoxelWorldProperty.js";

/**
 * Set `AVoxelWorld::bEnableLumen` — Lumen global illumination on voxel
 * meshes (default false; expensive). `value` is a boolean.
 *
 * Header: `Voxel/Public/VoxelWorld.h` (`bool bEnableLumen`).
 * Wraps one host call: `level.set_actor_property`.
 */
export default class SetEnableLumen extends SetVoxelWorldProperty {
  get taskName(): string { return "voxel.set_world_enable_lumen"; }
  protected readonly propertyName = "bEnableLumen";
}
