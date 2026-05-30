import { StampBuilderOp, HEIGHT_BLEND_MODE, type StampOptions } from "./StampBuilderOp.js";

/**
 * Build a height-spline stamp from a UVoxelHeightSplineGraph (`source`) and apply
 * it to a stamp actor. `VoxelHeightSplineStamp.Make`. Wraps editor.execute_python.
 */
export default class SetHeightSplineStamp extends StampBuilderOp<StampOptions> {
  get taskName(): string { return "voxel.set_height_spline_stamp"; }
  protected readonly makeClass = "VoxelHeightSplineStamp_K2";
  protected readonly blendEnumType = "VoxelHeightBlendMode";
  protected readonly blendModeMap = HEIGHT_BLEND_MODE;
  protected readonly defaultBlend = "max";
  protected readonly boundsExtension = 0.1;
  protected leadingArgs(): string[] { return ["graph=_src"]; }
}
