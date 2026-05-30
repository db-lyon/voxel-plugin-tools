import { StampBuilderOp, VOLUME_BLEND_MODE, type StampOptions } from "./StampBuilderOp.js";

/**
 * Build a mesh stamp from a UVoxelStaticMesh (`source`) and apply it to a stamp
 * actor (tricubic sampling on). `VoxelMeshStamp.Make`. Wraps editor.execute_python.
 */
export default class SetMeshStamp extends StampBuilderOp<StampOptions> {
  get taskName(): string { return "voxel.set_mesh_stamp"; }
  protected readonly makeClass = "VoxelMeshStamp_K2";
  protected readonly blendEnumType = "VoxelVolumeBlendMode";
  protected readonly blendModeMap = VOLUME_BLEND_MODE;
  protected readonly defaultBlend = "additive";
  protected readonly boundsExtension = 1.0;
  protected leadingArgs(): string[] {
    return ["new_mesh=_src", "surface_type=None", "use_tricubic=True"];
  }
}
