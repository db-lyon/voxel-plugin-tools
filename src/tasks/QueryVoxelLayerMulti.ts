import { UeMcpTask, type TaskResult } from "ue-mcp/task";
import { pyVec3, pyFloat, pyStr, type Vec3 } from "./_python.js";

interface Options {
  /** World-space points to sample. */
  positions: Vec3[];
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
 * Batch-sample many points of a voxel layer in one call — returns the
 * array of `FVoxelQueryResult` as JSON on a `VOXEL_QUERY_RESULT=` line. Cheaper
 * than N single queries (one layer resolution, one round-trip).
 *
 * `VoxelQueryBlueprintLibrary::MultiQueryVoxelLayer` is a static library
 * function taking an `FVoxelStackLayer` + array of positions and returning
 * out-params — wraps one `editor.execute_python` call.
 *
 * Header: `Voxel/Public/VoxelQueryBlueprintLibrary.h`.
 */
export default class QueryVoxelLayerMulti extends UeMcpTask<Options> {
  get taskName(): string { return "voxel.multi_query_voxel_layer"; }

  protected validate(): void {
    if (!Array.isArray(this.options.positions) || this.options.positions.length === 0) {
      throw new Error("positions must be a non-empty array of {x,y,z}");
    }
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
    const positions = o.positions.map(pyVec3).join(", ");
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
      `_success, _results = unreal.VoxelQueryBlueprintLibrary_BlueprintOnly.multi_query_voxel_layer(`,
      `    _wco, _sl, [${positions}], [], gradient_step=${pyFloat(o.gradientStep ?? 100)})`,
      `_f = lambda x: None if (isinstance(x, float) and x != x) else x`,
      `def _ser(r):`,
      `    _n = r.get_editor_property("normal")`,
      `    _st = r.get_editor_property("surface_type")`,
      `    return {"value": _f(r.get_editor_property("value")),`,
      `            "surface_type": (_st.get_name() if _st is not None else None),`,
      `            "normal": {"x": _f(_n.x), "y": _f(_n.y), "z": _f(_n.z)}}`,
      `_out = {"success": bool(_success), "results": [_ser(r) for r in _results]}`,
      `print("VOXEL_QUERY_RESULT=" + json.dumps(_out))`,
    ].join("\n");
    return this.call("editor.execute_python", { code });
  }
}
