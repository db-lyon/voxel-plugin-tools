import { StampBuilderOp, HEIGHT_BLEND_MODE, type StampOptions } from "./StampBuilderOp.js";

/**
 * Build a heightmap stamp from a UVoxelHeightmap (`source`) and apply it to a
 * stamp actor. `VoxelHeightmapStamp.Make`. Wraps editor.execute_python.
 */
export default class SetHeightmapStamp extends StampBuilderOp<StampOptions> {
  get taskName(): string { return "voxel.set_heightmap_stamp"; }
  protected readonly makeClass = "VoxelHeightmapStamp_K2";
  protected readonly blendEnumType = "VoxelHeightBlendMode";
  protected readonly blendModeMap = HEIGHT_BLEND_MODE;
  protected readonly defaultBlend = "max";
  protected readonly boundsExtension = 0.1;
  protected leadingArgs(): string[] {
    return ["heightmap=_src", "default_surface_type=None", "weightmap_surface_types=[]"];
  }
}
