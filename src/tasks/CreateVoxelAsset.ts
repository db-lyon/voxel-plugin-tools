import { UeMcpTask, type TaskResult } from "ue-mcp/task";
import { pyStr } from "./_python.js";

/** Friendly type key -> Voxel UObject asset class. */
const ASSET_CLASS: Record<string, string> = {
  surface_type: "VoxelSurfaceTypeAsset",
  smart_surface_type: "VoxelSmartSurfaceType",
  layer_stack: "VoxelLayerStack",
  mega_material: "VoxelMegaMaterial",
  heightmap: "VoxelHeightmap",
  static_mesh: "VoxelStaticMesh",
  height_graph: "VoxelHeightGraph",
  volume_graph: "VoxelVolumeGraph",
  scatter_graph: "VoxelScatterGraph",
  height_spline_graph: "VoxelHeightSplineGraph",
  volume_spline_graph: "VoxelVolumeSplineGraph",
  height_sculpt_graph: "VoxelHeightSculptGraph",
  volume_sculpt_graph: "VoxelVolumeSculptGraph",
  surface_type_graph: "VoxelSurfaceTypeGraph",
  height_sculpt_save: "VoxelHeightSculptSaveAsset",
  volume_sculpt_save: "VoxelVolumeSculptSaveAsset",
  pcg_graph: "VoxelPCGGraph",
};

interface Options {
  /** Asset name (no path). */
  name: string;
  /** Which Voxel asset to create (see ASSET_CLASS keys). */
  type: string;
  /** Content path. Default /Game. */
  packagePath?: string;
}

/**
 * Create a new Voxel asset of a given type (surface type, layer stack, graph,
 * heightmap, static mesh, …). The host asset tools can't create a UObject asset
 * by class, so this wraps `editor.execute_python` (AssetTools.create_asset).
 * Returns the new asset's object path on a `VOXEL_CREATED_ASSET=` line.
 */
export default class CreateVoxelAsset extends UeMcpTask<Options> {
  get taskName(): string { return "voxel.create_asset"; }

  protected validate(): void {
    if (!this.options.name) throw new Error("name is required");
    if (!this.options.type || !ASSET_CLASS[this.options.type]) {
      throw new Error(`type must be one of ${Object.keys(ASSET_CLASS).join("|")}`);
    }
  }

  async execute(): Promise<TaskResult> {
    const o = this.options;
    const cls = ASSET_CLASS[o.type];
    const path = o.packagePath ?? "/Game";
    const code = [
      "import unreal",
      `_a = unreal.AssetToolsHelpers.get_asset_tools().create_asset(${pyStr(o.name)}, ${pyStr(path)}, unreal.${cls}, None)`,
      `if _a is None:`,
      `    raise Exception("failed to create ${cls} at ${path}/${o.name}")`,
      `unreal.EditorAssetLibrary.save_loaded_asset(_a)`,
      `print("VOXEL_CREATED_ASSET=" + _a.get_path_name())`,
    ].join("\n");
    return this.call("editor.execute_python", { code });
  }
}
