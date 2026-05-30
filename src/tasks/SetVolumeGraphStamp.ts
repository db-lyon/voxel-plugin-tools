import { StampBuilderOp, VOLUME_BLEND_MODE, type StampOptions } from "./StampBuilderOp.js";

/**
 * Build a volume-graph stamp from a UVoxelVolumeGraph (`source`) and apply it to
 * a stamp actor. `VoxelVolumeGraphStamp.Make`. Wraps editor.execute_python.
 */
export default class SetVolumeGraphStamp extends StampBuilderOp<StampOptions> {
  get taskName(): string { return "voxel.set_volume_graph_stamp"; }
  protected readonly makeClass = "VoxelVolumeGraphStamp_K2";
  protected readonly blendEnumType = "VoxelVolumeBlendMode";
  protected readonly blendModeMap = VOLUME_BLEND_MODE;
  protected readonly defaultBlend = "additive";
  protected readonly boundsExtension = 1.0;
  protected leadingArgs(): string[] { return ["graph=_src"]; }
}
