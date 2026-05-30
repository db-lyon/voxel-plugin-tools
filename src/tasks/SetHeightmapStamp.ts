import { StampBuilderOp, HEIGHT_BLEND_MODE, type StampOptions } from "./StampBuilderOp.js";
import { pyStr } from "./_python.js";

interface Options extends StampOptions {
  /** Object path to the base UVoxelSurfaceTypeInterface used where no weightmap applies. */
  defaultSurfaceType?: string;
  /** Per-weightmap-channel surface overrides: { index, surfaceType (asset path) }. */
  weightmaps?: { index: number; surfaceType: string }[];
}

/**
 * Build a heightmap stamp from a UVoxelHeightmap (`source`) and apply it to a
 * stamp actor. Supports a base surface type plus per-weightmap-channel surface
 * overrides for multi-material terrain. `VoxelHeightmapStamp.Make`.
 * Wraps editor.execute_python.
 */
export default class SetHeightmapStamp extends StampBuilderOp<Options> {
  get taskName(): string { return "voxel.set_heightmap_stamp"; }
  protected readonly makeClass = "VoxelHeightmapStamp_K2";
  protected readonly blendEnumType = "VoxelHeightBlendMode";
  protected readonly blendModeMap = HEIGHT_BLEND_MODE;
  protected readonly defaultBlend = "max";
  protected readonly boundsExtension = 0.1;

  protected setupLines(): string[] {
    const o = this.options;
    const lines: string[] = [
      `_dst = ${o.defaultSurfaceType ? `unreal.load_asset(${pyStr(o.defaultSurfaceType)})` : "None"}`,
      `_wms = []`,
    ];
    for (const w of o.weightmaps ?? []) {
      lines.push(
        `_e = unreal.VoxelHeightmapStampWeightmapSurfaceType()`,
        `_e.set_editor_property("index", ${Number(w.index)})`,
        `_e.set_editor_property("surface_type", unreal.load_asset(${pyStr(w.surfaceType)}))`,
        `_wms.append(_e)`,
      );
    }
    return lines;
  }

  protected leadingArgs(): string[] {
    return ["heightmap=_src", "default_surface_type=_dst", "weightmap_surface_types=_wms"];
  }
}
