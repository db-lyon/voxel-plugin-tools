import { StampBuilderOp, HEIGHT_BLEND_MODE, type StampOptions } from "./StampBuilderOp.js";

/**
 * Build a height-graph stamp from a UVoxelHeightGraph (`source`) and apply it to
 * a stamp actor. `VoxelHeightGraphStamp.Make`. Wraps editor.execute_python.
 */
export default class SetHeightGraphStamp extends StampBuilderOp<StampOptions> {
  get taskName(): string { return "voxel.set_height_graph_stamp"; }
  protected readonly makeClass = "VoxelHeightGraphStamp_K2";
  protected readonly blendEnumType = "VoxelHeightBlendMode";
  protected readonly blendModeMap = HEIGHT_BLEND_MODE;
  protected readonly defaultBlend = "max";
  protected readonly boundsExtension = 0.1;
  protected leadingArgs(): string[] { return ["graph=_src"]; }
}
