import { SetVoxelWorldProperty } from "./SetVoxelWorldProperty.js";

/**
 * Set `AVoxelWorld::MegaMaterial` — the `UVoxelMegaMaterial` asset that
 * drives surface shading (per-surface-type materials + blending). `value` is
 * the asset object path.
 *
 * Header: `Voxel/Public/VoxelWorld.h` (`UVoxelMegaMaterial* MegaMaterial`).
 * Wraps one host call: `level.set_actor_property`.
 */
export default class SetMegaMaterial extends SetVoxelWorldProperty {
  get taskName(): string { return "voxel.set_world_mega_material"; }
  protected readonly propertyName = "MegaMaterial";
}
