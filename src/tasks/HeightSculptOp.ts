import { SculptOp, type SculptActorOptions } from "./SculptOp.js";

export type HeightSculptOptions = SculptActorOptions;

/**
 * Base for height-sculpt ops on an `AVoxelHeightSculptActor`. Binds the
 * `VoxelHeightSculptBlueprintLibrary_BlueprintOnly` class; subclasses bake the
 * function name + marshalled args. Note: height-sculpt centres are 2D
 * (`FVector2D`), unlike the 3D volume ops.
 *
 * Header: `Voxel/Public/Sculpt/Height/VoxelHeightSculptBlueprintLibrary.h`.
 */
export abstract class HeightSculptOp<TOpts extends HeightSculptOptions> extends SculptOp<TOpts> {
  protected readonly libraryClass = "VoxelHeightSculptBlueprintLibrary_BlueprintOnly";
}
