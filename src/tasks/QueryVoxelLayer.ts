import { UeMcpTask, type TaskResult } from "ue-mcp/task";
import { pyVec3, pyFloat, pyStr, type Vec3 } from "./_python.js";

interface Options {
  /** World-space point to sample. */
  position: Vec3;
  /** Object path to the UVoxelLayerStack. Default: the bundled DefaultStack. */
  stack?: string;
  /** Which layer array to read from the stack. Default: volume. */
  layerType?: "height" | "volume";
  /** Index into that layer array. Default 0. */
  layerIndex?: number;
  /** Finite-difference step (cm) for the surface normal. Default 100. */
  gradientStep?: number;
}

/**
 * Sample one point of a voxel layer — returns the `FVoxelQueryResult`
 * (value, surface bool, normal) as JSON on a `VOXEL_QUERY_RESULT=` line.
 *
 * `VoxelQueryBlueprintLibrary::QueryVoxelLayer` is a static library function
 * taking an `FVoxelStackLayer` ({stack, layer}) and returning out-params — the
 * host `invoke_function` can't express that, so this wraps one
 * `editor.execute_python` call: it resolves the layer from the stack asset,
 * runs the query against the editor world, and serialises the result.
 *
 * Header: `Voxel/Public/VoxelQueryBlueprintLibrary.h`.
 */
export default class QueryVoxelLayer extends UeMcpTask<Options> {
  get taskName(): string { return "voxel.query_voxel_layer"; }

  protected validate(): void {
    if (!this.options.position) throw new Error("position is required");
    const lt = this.options.layerType ?? "volume";
    if (lt !== "height" && lt !== "volume") {
      throw new Error("layerType must be 'height' or 'volume'");
    }
  }

  async execute(): Promise<TaskResult> {
    const o = this.options;
    const stack = o.stack ?? "/Voxel/Default/DefaultStack.DefaultStack";
    const layersProp = (o.layerType ?? "volume") === "height" ? "height_layers" : "volume_layers";
    const idx = o.layerIndex ?? 0;
    const code = [
      "import unreal, json",
      `_stack = unreal.load_asset(${pyStr(stack)})`,
      `if _stack is None:`,
      `    raise Exception("stack not found: " + ${pyStr(stack)})`,
      `_layers = _stack.get_editor_property(${pyStr(layersProp)})`,
      `if _layers is None or ${idx} >= len(_layers):`,
      `    raise Exception("layer index ${idx} out of range for ${layersProp}")`,
      `_sl = unreal.VoxelStackLayer()`,
      `_sl.set_editor_property("stack", _stack)`,
      `_sl.set_editor_property("layer", _layers[${idx}])`,
      `_wco = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()`,
      `_success, _result = unreal.VoxelQueryBlueprintLibrary_BlueprintOnly.query_voxel_layer(`,
      `    _wco, _sl, ${pyVec3(o.position)}, [], gradient_step=${pyFloat(o.gradientStep ?? 100)})`,
      `_n = _result.get_editor_property("normal")`,
      // NaN -> None so the line is valid JSON (value/normal are NaN where the
      // layer has no surface at the sample point, e.g. no runtime world yet).
      `_f = lambda x: None if (isinstance(x, float) and x != x) else x`,
      `_st = _result.get_editor_property("surface_type")`,
      `_out = {"success": bool(_success), "value": _f(_result.get_editor_property("value")),`,
      `        "surface_type": (_st.get_name() if _st is not None else None),`,
      `        "normal": {"x": _f(_n.x), "y": _f(_n.y), "z": _f(_n.z)}}`,
      `print("VOXEL_QUERY_RESULT=" + json.dumps(_out))`,
    ].join("\n");
    return this.call("editor.execute_python", { code });
  }
}
