import { SetVoxelWorldProperty } from "./SetVoxelWorldProperty.js";

/**
 * Set `AVoxelWorld::LayerStack` — the `UVoxelLayerStack` asset that
 * defines what the world generates/renders. `value` is the asset object path
 * (e.g. `/Voxel/Default/DefaultStack.DefaultStack`). Without a stack the world
 * renders nothing.
 *
 * Header: `Voxel/Public/VoxelWorld.h` (`UVoxelLayerStack* LayerStack`).
 * Wraps one host call: `level.set_actor_property`.
 */
export default class SetLayerStack extends SetVoxelWorldProperty {
  get taskName(): string { return "voxel.set_world_layer_stack"; }
  protected readonly propertyName = "LayerStack";
}
