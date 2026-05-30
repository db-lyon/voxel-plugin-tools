import { UeMcpTask, type TaskResult } from "ue-mcp/task";
import { pyVec2, pyFloat, pyStr, type Vec2 } from "./_python.js";

interface Options {
  /** Object path to a UTextureRenderTarget2D to write into. */
  renderTarget: string;
  /** World-space 2D zone to sample. */
  min: Vec2;
  max: Vec2;
  /** UVoxelLayerStack object path. Default: the bundled DefaultStack. */
  stack?: string;
  /** Index into the stack's height_layers. Default 0. */
  layerIndex?: number;
  /** What to write per pixel: the sampled height, or a constant. Default height. */
  query?: "height" | "constant";
  /** Value written when query=constant. Default 0. */
  constant?: number;
}

/**
 * Query a voxel height layer over a 2D zone and write it into a render target
 * (RGB = the chosen value, A = 1). Returns success on a `VOXEL_EXPORT_OK=` line.
 *
 * `VoxelQueryBlueprintLibrary::ExportVoxelDataToRenderTarget` is static and
 * takes an `FVoxelStackHeightLayer` + `FVoxelBox2D` + `FVoxelColorQuery` (built
 * from `QueryHeight`/`MakeConstant`) — wraps one `editor.execute_python` call.
 *
 * Header: `Voxel/Public/VoxelQueryBlueprintLibrary.h`.
 */
export default class ExportVoxelDataToRenderTarget extends UeMcpTask<Options> {
  get taskName(): string { return "voxel.export_voxel_data_to_render_target"; }

  protected validate(): void {
    if (!this.options.renderTarget) throw new Error("renderTarget (asset path) is required");
    if (!this.options.min || !this.options.max) throw new Error("min and max (2D zone) are required");
    const q = this.options.query ?? "height";
    if (q !== "height" && q !== "constant") throw new Error("query must be 'height' or 'constant'");
  }

  async execute(): Promise<TaskResult> {
    const o = this.options;
    const stack = o.stack ?? "/Voxel/Default/DefaultStack.DefaultStack";
    const idx = o.layerIndex ?? 0;
    const channel = (o.query ?? "height") === "constant"
      ? `unreal.VoxelQueryBlueprintLibrary.make_constant(${pyFloat(o.constant ?? 0)})`
      : `unreal.VoxelQueryBlueprintLibrary.query_height()`;
    const code = [
      "import unreal",
      `_rt = unreal.load_asset(${pyStr(o.renderTarget)})`,
      `if _rt is None:`,
      `    raise Exception("render target not found: " + ${pyStr(o.renderTarget)})`,
      `_stack = unreal.load_asset(${pyStr(stack)})`,
      `if _stack is None:`,
      `    raise Exception("stack not found: " + ${pyStr(stack)})`,
      `_hls = _stack.get_editor_property("height_layers")`,
      `if _hls is None or ${idx} >= len(_hls):`,
      `    raise Exception("height layer index ${idx} out of range")`,
      `_layer = unreal.VoxelStackHeightLayer()`,
      `_layer.set_editor_property("stack", _stack)`,
      `_layer.set_editor_property("layer", _hls[${idx}])`,
      `_zone = unreal.VoxelBox2D()`,
      `_zone.set_editor_property("min", ${pyVec2(o.min)})`,
      `_zone.set_editor_property("max", ${pyVec2(o.max)})`,
      `_chan = ${channel}`,
      `_one = unreal.VoxelQueryBlueprintLibrary.make_constant(1.0)`,
      `_cq = unreal.VoxelColorQuery()`,
      `_cq.set_editor_property("r", _chan)`,
      `_cq.set_editor_property("g", _chan)`,
      `_cq.set_editor_property("b", _chan)`,
      `_cq.set_editor_property("a", _one)`,
      `_wco = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()`,
      `_ok = unreal.VoxelQueryBlueprintLibrary_BlueprintOnly.export_voxel_data_to_render_target(`,
      `    _wco, _rt, _layer, _zone, _cq)`,
      `print("VOXEL_EXPORT_OK=" + str(bool(_ok)))`,
    ].join("\n");
    return this.call("editor.execute_python", { code });
  }
}
