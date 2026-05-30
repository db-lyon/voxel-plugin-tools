import { UeMcpTask, type TaskResult } from "ue-mcp/task";
import { withResolvedActor, pyStr, pyVec3, pyRotator, pyFloat, pyEnum, type Vec3, type Rot3 } from "./_python.js";

const HEIGHT_BLEND_MODE: Record<string, string> = { max: "MAX", min: "MIN", override: "OVERRIDE" };

interface Options {
  /** Label of an AVoxelStampActor (or actor with a UVoxelStampComponent). */
  actorLabel: string;
  /** Object path to a UVoxelHeightmap asset. */
  heightmap: string;
  location?: Vec3;
  rotation?: Rot3;
  scale?: Vec3;
  blendMode?: "max" | "min" | "override";
  priority?: number;
  smoothness?: number;
}

/**
 * Build a heightmap stamp from a UVoxelHeightmap and apply it to a stamp actor
 * (VoxelHeightmapStamp.Make -> AVoxelStampActor::SetStamp -> UpdateStamp).
 * The 16-arg make is supplied with sensible defaults for the specialised params
 * (no weightmaps, default layer, identity metadata/seed). Wraps editor.execute_python.
 *
 * Header: `Voxel/Public/Height/VoxelHeightmapStamp.h`, `Voxel/Public/VoxelStampActor.h`.
 */
export default class SetHeightmapStamp extends UeMcpTask<Options> {
  get taskName(): string { return "voxel.set_heightmap_stamp"; }

  protected validate(): void {
    if (!this.options.actorLabel) throw new Error("actorLabel is required");
    if (!this.options.heightmap) throw new Error("heightmap (asset path) is required");
    const bm = this.options.blendMode ?? "max";
    if (!HEIGHT_BLEND_MODE[bm]) throw new Error("blendMode must be max|min|override");
  }

  async execute(): Promise<TaskResult> {
    const o = this.options;
    const hm = pyStr(o.heightmap);
    const loc = pyVec3(o.location ?? { x: 0, y: 0, z: 0 });
    const scale = pyVec3(o.scale ?? { x: 1, y: 1, z: 1 });
    const rot = pyRotator(o.rotation ?? { pitch: 0, yaw: 0, roll: 0 });
    const blend = pyEnum("VoxelHeightBlendMode", o.blendMode ?? "max", HEIGHT_BLEND_MODE);
    const code = withResolvedActor(o.actorLabel, [
      `_hm = unreal.load_asset(${hm})`,
      `if _hm is None:`,
      `    raise Exception("heightmap not found: " + ${hm})`,
      `_t = unreal.Transform()`,
      `_t.set_editor_property("translation", ${loc})`,
      `_t.set_editor_property("scale3d", ${scale})`,
      `_t.set_editor_property("rotation", ${rot}.quaternion())`,
      `_stamp = unreal.VoxelHeightmapStamp_K2.make(`,
      `    heightmap=_hm, default_surface_type=None, weightmap_surface_types=[], layer=None,`,
      `    blend_mode=${blend}, additional_layers=[], transform=_t,`,
      `    behavior=unreal.VoxelStampBehavior.AFFECT_ALL, priority=${pyFloat(o.priority ?? 0)},`,
      `    smoothness=${pyFloat(o.smoothness ?? 100)}, metadata_overrides=unreal.VoxelMetadataOverrides(),`,
      `    stamp_seed=unreal.VoxelExposedSeed(), lod_range=unreal.Int32Interval(min=0, max=32),`,
      `    disable_stamp_selection=False, apply_on_void=True, bounds_extension=0.1)`,
      `_actor.set_stamp(_stamp)`,
      `_actor.update_stamp()`,
      `print("voxel.set_heightmap_stamp: applied to " + _actor.get_actor_label())`,
    ]);
    return this.call("editor.execute_python", { code });
  }
}
