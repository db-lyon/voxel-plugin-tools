import { UeMcpTask, type TaskResult } from "ue-mcp/task";
import { withResolvedActor, pyStr, pyVec3, pyRotator, pyFloat, pyEnum, type Vec3, type Rot3 } from "./_python.js";

export const HEIGHT_BLEND_MODE: Record<string, string> = { max: "MAX", min: "MIN", override: "OVERRIDE" };
export const VOLUME_BLEND_MODE: Record<string, string> = {
  additive: "ADDITIVE", subtractive: "SUBTRACTIVE", intersect: "INTERSECT", override: "OVERRIDE",
};

export interface StampOptions {
  /** Label of an AVoxelStampActor to apply the stamp to. */
  actorLabel: string;
  /** Object path to the source asset (heightmap / graph / mesh, per the tool). */
  source: string;
  location?: Vec3;
  rotation?: Rot3;
  scale?: Vec3;
  blendMode?: string;
  priority?: number;
  smoothness?: number;
}

/**
 * Base for "build a stamp ref and apply it to a stamp actor" tasks. Every
 * `Voxel*Stamp_K2.make` shares a 13-arg tail (layer/blend/transform/behavior/
 * priority/smoothness/metadata/seed/lod/…); subclasses bake the make-class, the
 * blend-mode enum, and the source-specific leading args. The body wraps one
 * `editor.execute_python` call: load source -> build transform -> make -> SetStamp
 * -> UpdateStamp.
 */
export abstract class StampBuilderOp<TOpts extends StampOptions> extends UeMcpTask<TOpts> {
  /** e.g. `VoxelHeightGraphStamp_K2`. */
  protected abstract readonly makeClass: string;
  /** `VoxelHeightBlendMode` | `VoxelVolumeBlendMode`. */
  protected abstract readonly blendEnumType: string;
  protected abstract readonly blendModeMap: Record<string, string>;
  protected abstract readonly defaultBlend: string;
  protected abstract readonly boundsExtension: number;
  /** make() args specific to this stamp type, referencing `_src` (the loaded source). */
  protected abstract leadingArgs(): string[];

  protected validate(): void {
    if (!this.options.actorLabel) throw new Error("actorLabel is required");
    if (!this.options.source) throw new Error("source (asset path) is required");
    const bm = this.options.blendMode ?? this.defaultBlend;
    if (!this.blendModeMap[bm]) {
      throw new Error(`blendMode must be one of ${Object.keys(this.blendModeMap).join("|")}`);
    }
  }

  async execute(): Promise<TaskResult> {
    const o = this.options;
    const src = pyStr(o.source);
    const makeArgs = [
      ...this.leadingArgs(),
      "layer=None",
      `blend_mode=${pyEnum(this.blendEnumType, o.blendMode ?? this.defaultBlend, this.blendModeMap)}`,
      "additional_layers=[]",
      "transform=_t",
      "behavior=unreal.VoxelStampBehavior.AFFECT_ALL",
      `priority=${pyFloat(o.priority ?? 0)}`,
      `smoothness=${pyFloat(o.smoothness ?? 100)}`,
      "metadata_overrides=unreal.VoxelMetadataOverrides()",
      "stamp_seed=unreal.VoxelExposedSeed()",
      "lod_range=unreal.Int32Interval(min=0, max=32)",
      "disable_stamp_selection=False",
      "apply_on_void=True",
      `bounds_extension=${pyFloat(this.boundsExtension)}`,
    ].join(", ");
    const code = withResolvedActor(o.actorLabel, [
      `_src = unreal.load_asset(${src})`,
      `if _src is None:`,
      `    raise Exception("source asset not found: " + ${src})`,
      `_t = unreal.Transform()`,
      `_t.set_editor_property("translation", ${pyVec3(o.location ?? { x: 0, y: 0, z: 0 })})`,
      `_t.set_editor_property("scale3d", ${pyVec3(o.scale ?? { x: 1, y: 1, z: 1 })})`,
      `_t.set_editor_property("rotation", ${pyRotator(o.rotation ?? { pitch: 0, yaw: 0, roll: 0 })}.quaternion())`,
      `_stamp = unreal.${this.makeClass}.make(${makeArgs})`,
      `_actor.set_stamp(_stamp)`,
      `_actor.update_stamp()`,
      `print(${pyStr(`${this.taskName}: applied to `)} + _actor.get_actor_label())`,
    ]);
    return this.call("editor.execute_python", { code });
  }
}
