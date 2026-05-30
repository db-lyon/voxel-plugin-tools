import { SculptOp, type SculptActorOptions } from "./SculptOp.js";

export type VolumeSculptOptions = SculptActorOptions;

/**
 * Base for volume-sculpt ops on an `AVoxelVolumeSculptActor`. Binds the
 * `VoxelVolumeSculptBlueprintLibrary_BlueprintOnly` class; subclasses bake the
 * function name + marshalled args.
 *
 * Header: `Voxel/Public/Sculpt/Volume/VoxelVolumeSculptBlueprintLibrary.h`.
 */
export abstract class VolumeSculptOp<TOpts extends VolumeSculptOptions> extends SculptOp<TOpts> {
  protected readonly libraryClass = "VoxelVolumeSculptBlueprintLibrary_BlueprintOnly";
}
