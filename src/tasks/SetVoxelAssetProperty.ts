import { UeMcpTask, type TaskResult } from "ue-mcp/task";
import { pyStr } from "./_python.js";

interface Options {
  /** Object path to the Voxel asset to edit. */
  assetPath: string;
  /** snake_case editor property name (e.g. "material", "blend_smoothness", "scale_xy"). */
  propertyName: string;
  /**
   * The value. Carried as a string over the MCP boundary (the host plugin
   * schema has no union/any type), though a number or boolean is still accepted
   * if a caller supplies one directly. When `valueIsAsset` is false, a string
   * that looks numeric or boolean is coerced to the matching Python type so the
   * underlying `set_editor_property` gets a real float/int/bool; an asset object
   * path when `valueIsAsset` is true.
   */
  value: string | number | boolean;
  /** If true, `value` is an object path to load_asset and assign (e.g. a Material). */
  valueIsAsset?: boolean;
}

/**
 * Set one editor property on a Voxel asset and save it (e.g. a surface type's
 * `material` / `blend_smoothness`, a heightmap's `scale_xy`). Wraps
 * `editor.execute_python` (load_asset -> set_editor_property -> save). Use
 * `valueIsAsset` when the property holds a UObject reference.
 */
export default class SetVoxelAssetProperty extends UeMcpTask<Options> {
  get taskName(): string { return "voxel.set_asset_property"; }

  protected validate(): void {
    if (!this.options.assetPath) throw new Error("assetPath is required");
    if (!this.options.propertyName) throw new Error("propertyName is required");
    if (this.options.value === undefined) throw new Error("value is required");
  }

  async execute(): Promise<TaskResult> {
    const o = this.options;
    let valueExpr: string;
    if (o.valueIsAsset) {
      valueExpr = `unreal.load_asset(${pyStr(String(o.value))})`;
    } else if (typeof o.value === "number") {
      valueExpr = String(o.value);
    } else if (typeof o.value === "boolean") {
      valueExpr = o.value ? "True" : "False";
    } else {
      // Arrives as a string over the MCP boundary; coerce numeric/boolean-looking
      // strings to a real Python literal so float/int/bool properties get the
      // right type. A genuine string (incl. one that isn't a clean number/bool)
      // is passed through quoted.
      const s = o.value.trim();
      if (/^(true|false)$/i.test(s)) {
        valueExpr = /^true$/i.test(s) ? "True" : "False";
      } else if (/^[+-]?(\d+\.?\d*|\.\d+)$/.test(s)) {
        valueExpr = s;
      } else {
        valueExpr = pyStr(o.value);
      }
    }
    const ap = pyStr(o.assetPath);
    const code = [
      "import unreal",
      `_a = unreal.load_asset(${ap})`,
      `if _a is None:`,
      `    raise Exception("asset not found: " + ${ap})`,
      `_a.set_editor_property(${pyStr(o.propertyName)}, ${valueExpr})`,
      `unreal.EditorAssetLibrary.save_loaded_asset(_a)`,
      `print("voxel.set_asset_property: " + ${ap} + "." + ${pyStr(o.propertyName)} + " set")`,
    ].join("\n");
    return this.call("editor.execute_python", { code });
  }
}
