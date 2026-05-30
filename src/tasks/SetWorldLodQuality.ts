import { UeMcpTask, type TaskResult } from "ue-mcp/task";
import { pyFloat } from "./_python.js";
import { withResolvedActor } from "./_python.js";

interface Range { min: number; max: number; }

interface Options {
  actorLabel: string;
  /** Editor LOD-quality range (FFloatInterval min/max; higher = finer, heavier). */
  editorQuality?: Range;
  /** Runtime/PIE LOD-quality range (FFloatInterval min/max). */
  gameQuality?: Range;
  /** Use the game quality even in the editor. */
  alwaysUseGameQuality?: boolean;
}

/**
 * Set fields of `AVoxelWorld::LODQuality` (FVoxelLODQuality) — the detail-vs-
 * performance lever beyond VoxelSize. `editor_quality` / `game_quality` are
 * FFloatInterval (min/max) ranges; `always_use_game_quality` is a bool. Wraps
 * editor.execute_python. Header: Voxel/Public/VoxelWorld.h.
 */
export default class SetWorldLodQuality extends UeMcpTask<Options> {
  get taskName(): string { return "voxel.set_world_lod_quality"; }

  protected validate(): void {
    if (!this.options.actorLabel) throw new Error("actorLabel is required");
    if (this.options.editorQuality === undefined
      && this.options.gameQuality === undefined
      && this.options.alwaysUseGameQuality === undefined) {
      throw new Error("provide at least one of editorQuality, gameQuality, alwaysUseGameQuality");
    }
    for (const r of [this.options.editorQuality, this.options.gameQuality]) {
      if (r !== undefined && (typeof r.min !== "number" || typeof r.max !== "number")) {
        throw new Error("editorQuality/gameQuality must be { min, max }");
      }
    }
  }

  async execute(): Promise<TaskResult> {
    const o = this.options;
    const interval = (r: Range) => `unreal.FloatInterval(min=${pyFloat(r.min)}, max=${pyFloat(r.max)})`;
    const sets: string[] = [];
    if (o.editorQuality !== undefined) sets.push(`_lq.set_editor_property("editor_quality", ${interval(o.editorQuality)})`);
    if (o.gameQuality !== undefined) sets.push(`_lq.set_editor_property("game_quality", ${interval(o.gameQuality)})`);
    if (o.alwaysUseGameQuality !== undefined) {
      sets.push(`_lq.set_editor_property("always_use_game_quality", ${o.alwaysUseGameQuality ? "True" : "False"})`);
    }
    const code = withResolvedActor(o.actorLabel, [
      `_lq = _actor.get_editor_property("lod_quality")`,
      ...sets,
      `_actor.set_editor_property("lod_quality", _lq)`,
      `print("voxel.set_world_lod_quality: set on " + _actor.get_actor_label())`,
    ]);
    return this.call("editor.execute_python", { code });
  }
}
