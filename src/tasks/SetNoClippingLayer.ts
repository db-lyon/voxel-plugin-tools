import { UeMcpTask, type TaskResult } from "ue-mcp/task";
import { withResolvedActor, pyStr } from "./_python.js";

interface Options {
  actorLabel: string;
  /** Name of the UVoxelNoClippingComponent on the actor. */
  componentName: string;
  /** UVoxelLayerStack object path the layer comes from. Default: DefaultStack. */
  stack?: string;
  layerType?: "height" | "volume";
  layerIndex?: number;
  /** Auto-teleport the owner above solid ground. Default true. */
  autoAdjustPlayer?: boolean;
}

/**
 * Configure a UVoxelNoClippingComponent: set the voxel layer it tests against
 * (the "solid ground" layer) and whether to auto-adjust the owner. Wraps
 * editor.execute_python (builds an FVoxelStackLayer from a stack asset).
 */
export default class SetNoClippingLayer extends UeMcpTask<Options> {
  get taskName(): string { return "voxel.set_no_clipping_layer"; }

  protected validate(): void {
    if (!this.options.actorLabel) throw new Error("actorLabel is required");
    if (!this.options.componentName) throw new Error("componentName is required");
    const lt = this.options.layerType ?? "volume";
    if (lt !== "height" && lt !== "volume") throw new Error("layerType must be 'height' or 'volume'");
  }

  async execute(): Promise<TaskResult> {
    const o = this.options;
    const stack = pyStr(o.stack ?? "/Voxel/Default/DefaultStack.DefaultStack");
    const layersProp = (o.layerType ?? "volume") === "height" ? "height_layers" : "volume_layers";
    const idx = o.layerIndex ?? 0;
    const auto = (o.autoAdjustPlayer ?? true) ? "True" : "False";
    const name = pyStr(o.componentName);
    const code = withResolvedActor(o.actorLabel, [
      `_comp = next((c for c in _actor.get_components_by_class(unreal.VoxelNoClippingComponent) if c.get_name() == ${name}), None)`,
      `if _comp is None:`,
      `    raise Exception("no-clipping component not found: " + ${name})`,
      `_stack = unreal.load_asset(${stack})`,
      `if _stack is None:`,
      `    raise Exception("stack not found: " + ${stack})`,
      `_layers = _stack.get_editor_property("${layersProp}")`,
      `if _layers is None or ${idx} >= len(_layers):`,
      `    raise Exception("layer index ${idx} out of range for ${layersProp}")`,
      `_sl = unreal.VoxelStackLayer()`,
      `_sl.set_editor_property("stack", _stack)`,
      `_sl.set_editor_property("layer", _layers[${idx}])`,
      `_comp.set_editor_property("layer", _sl)`,
      `_comp.set_editor_property("auto_adjust_player", ${auto})`,
      `print("voxel.set_no_clipping_layer: configured " + ${name})`,
    ]);
    return this.call("editor.execute_python", { code });
  }
}
