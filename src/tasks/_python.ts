/**
 * Helpers for generating UE Python passed to the host `editor.execute_python`
 * call. Voxel's sculpt/query operations are static `UBlueprintFunctionLibrary`
 * methods (on the UHT-generated `*_BlueprintOnly` classes) — the host
 * `editor.invoke_function` only targets actor/component instances, so static
 * calls go through Python. These helpers marshal typed task options into the
 * exact literal forms the `unreal` Python API expects.
 */

export interface Vec2 { x: number; y: number; }
export interface Vec3 { x: number; y: number; z: number; }
export interface Rot3 { pitch: number; yaw: number; roll: number; }

/** A Python numeric literal. UE coerces int→float for float params. */
export function pyFloat(n: number): string {
  if (typeof n !== "number" || !Number.isFinite(n)) {
    throw new Error(`expected a finite number, got ${JSON.stringify(n)}`);
  }
  return String(n);
}

/** `unreal.Vector(x, y, z)` from a {x,y,z}. */
export function pyVec3(v: Vec3): string {
  if (!v || typeof v.x !== "number" || typeof v.y !== "number" || typeof v.z !== "number") {
    throw new Error(`expected a {x,y,z} vector, got ${JSON.stringify(v)}`);
  }
  return `unreal.Vector(${pyFloat(v.x)}, ${pyFloat(v.y)}, ${pyFloat(v.z)})`;
}

/** `unreal.Vector2D(x, y)` from a {x,y}. */
export function pyVec2(v: Vec2): string {
  if (!v || typeof v.x !== "number" || typeof v.y !== "number") {
    throw new Error(`expected a {x,y} vector, got ${JSON.stringify(v)}`);
  }
  return `unreal.Vector2D(${pyFloat(v.x)}, ${pyFloat(v.y)})`;
}

/** `unreal.Rotator(roll=, pitch=, yaw=)` from a {pitch,yaw,roll}. */
export function pyRotator(r: Rot3): string {
  if (!r || typeof r.pitch !== "number" || typeof r.yaw !== "number" || typeof r.roll !== "number") {
    throw new Error(`expected a {pitch,yaw,roll} rotator, got ${JSON.stringify(r)}`);
  }
  return `unreal.Rotator(roll=${pyFloat(r.roll)}, pitch=${pyFloat(r.pitch)}, yaw=${pyFloat(r.yaw)})`;
}

/** A Python string literal (double-quoted, escaped). */
export function pyStr(s: string): string {
  return JSON.stringify(String(s));
}

/**
 * `unreal.<UnrealType>.<MEMBER>` from a friendly value, validated against an
 * allow-map (lower-cased input key → Python enum member).
 */
export function pyEnum(
  unrealType: string,
  value: string,
  allowed: Record<string, string>,
): string {
  const member = allowed[String(value).toLowerCase()];
  if (!member) {
    throw new Error(
      `invalid ${unrealType} value ${JSON.stringify(value)}; expected one of ${Object.keys(allowed).join(", ")}`,
    );
  }
  return `unreal.${unrealType}.${member}`;
}

export const SCULPT_MODE: Record<string, string> = { add: "ADD", remove: "REMOVE" };
export const LEVEL_TOOL_TYPE: Record<string, string> = {
  additive: "ADDITIVE",
  subtractive: "SUBTRACTIVE",
  both: "BOTH",
};
export const SDF_MERGE_MODE: Record<string, string> = {
  intersection: "INTERSECTION",
  override: "OVERRIDE",
  union: "UNION",
};

/**
 * Build a Python snippet that resolves an editor-level actor by label into the
 * local `_actor`, raising if absent, then runs `body` (which may reference
 * `_actor`). Keeps actor-resolution identical across every Python-backed task.
 */
export function withResolvedActor(actorLabel: string, body: string[]): string {
  const label = pyStr(actorLabel);
  return [
    "import unreal",
    "_eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)",
    `_actor = next((a for a in _eas.get_all_level_actors() if a.get_actor_label() == ${label}), None)`,
    "if _actor is None:",
    `    raise Exception("actor not found: " + ${label})`,
    ...body,
  ].join("\n");
}
