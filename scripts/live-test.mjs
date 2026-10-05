// Live smoke test: drives every native handler against a running editor and asserts the results.
// node scripts/live-test.mjs [path/to/project.uproject]   (default: the test project; open an empty map first)
//
// Calls go straight to the bridge (EditorBridge.call), not through the ue-mcp server, so nothing checks a call
// before the C++ handler does: every refusal below is the handler's own (RunHandler checks the contract first).
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import yaml from "js-yaml";

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const uproject = process.argv[2] ?? path.join(root, "tests", "voxel_plugin_tools", "voxel_plugin_tools.uproject");
const { EditorBridge } = await import(
  "file://" + path.join(root, "node_modules", "ue-mcp", "dist", "bridge", "bridge.js").replace(/\\/g, "/")
);

const bridge = new EditorBridge("127.0.0.1", 0);
bridge.setProjectContext(uproject);
await bridge.connect(10000);

// A fresh folder per run: existing assets are never overwritten, so runs must not share one.
const P = `/Game/VoxelSmoke/Run${Date.now()}`;
const ctx = {};
const results = [];
let failed = 0;

async function step(name, method, params, check = () => true, { expectError = false } = {}) {
  let reply;
  try {
    reply = await bridge.call(method, typeof params === "function" ? params() : params, 120000);
  } catch (e) {
    reply = { success: false, error: String(e?.message ?? e) };
  }
  if (expectError) {
    // expectError: true, or a RegExp the error text must match so the refusal is the expected one.
    const refused = reply?.success === false;
    const matches = !(expectError instanceof RegExp) || expectError.test(reply?.error ?? "");
    const ok = refused && matches;
    const why = !refused ? "expected an error" : `error did not match ${expectError}: ${reply?.error}`;
    if (!ok) failed++;
    results.push({ name, ok, why: ok ? "" : why });
    console.log(`${ok ? "PASS" : "FAIL"}  ${name}${ok ? "" : `  -> ${why}`}`);
    return reply;
  }
  let ok = reply?.success === true;
  let why = reply?.error ?? "";
  if (ok) {
    try {
      const verdict = check(reply);
      if (verdict !== true) { ok = false; why = verdict || "check failed"; }
    } catch (e) { ok = false; why = `check threw: ${e.message}`; }
  }
  if (!ok) failed++;
  results.push({ name, ok, why });
  console.log(`${ok ? "PASS" : "FAIL"}  ${name}${ok ? "" : `  -> ${why}`}`);
  return reply;
}

// Preflight
await step("shader_hooks_status", "voxel_shader_hooks_status", {}, (r) => (Array.isArray(r.groups) && r.groups.length > 0) || "no hook groups");

// Contracts: every manifest handler registered one, it declares save exactly when the handler mutates (RunHandler
// reads save only then), and the handler refuses each rule of it when called directly.
const manifest = yaml.load(fs.readFileSync(path.join(root, "ue-mcp.plugin.yml"), "utf8"));
const manifestHandlers = manifest.nativeModule.handlers;
let specs = {};
try {
  specs = (await bridge.call("get_bridge_capabilities", {}, 30000))?.pluginHandlerSpecs ?? {};
} catch (e) {
  console.log(`get_bridge_capabilities failed: ${e?.message ?? e}`);
}
for (const [method, h] of Object.entries(manifestHandlers)) {
  const spec = specs[method];
  const ok = !!spec && spec.params.some((p) => p.name === "save") === (h.effect === "mutate");
  if (!ok) failed++;
  const why = !spec ? "no contract in pluginHandlerSpecs" : `save declared ${!(h.effect === "mutate")} for effect ${h.effect}`;
  results.push({ name: `contract ${method}`, ok, why: ok ? "" : why });
  console.log(`${ok ? "PASS" : "FAIL"}  contract ${method}${ok ? "" : `  -> ${why}`}`);
}

// A value each declared shape accepts, so a call is valid apart from the one value a step breaks.
const sampleScalar = (type, rules) => {
  if (rules.literal !== undefined) return rules.literal;
  if (rules.enum?.length) return rules.enum[0];
  switch (type) {
    case "string": return "x";
    case "number": case "integer": return rules.minimum ?? rules.maximum ?? 0;
    case "boolean": return false;
    case "vec3": return { x: 0, y: 0, z: 0 };
    case "rotator": return { pitch: 0, yaw: 0, roll: 0 };
    case "color": return { r: 0, g: 0, b: 0 };
    default: return "x";
  }
};
const sampleFields = (fields) => Object.fromEntries(fields.filter((f) => f.required).map((f) => [f.name, sampleField(f)]));
function sampleField(f) {
  if (f.forms?.length) return f.forms[0] === "argMap" ? {} : f.forms[0] === "string" ? "x" : [];
  if (f.type === "array") return f.fields ? [sampleFields(f.fields)] : [];
  if (f.type === "object") return f.fields ? sampleFields(f.fields) : {};
  return sampleScalar(f.type, f);
}
function sampleParam(p) {
  if (p.oneOf) {
    const v = p.oneOf.variants[0];
    const one = { [p.oneOf.key]: v.tag, ...sampleFields(v.fields) };
    return p.type === "array" ? [one] : one;
  }
  if (p.type === "array") return p.fields ? [sampleFields(p.fields)] : [];
  return sampleField(p);
}
// Required parameters plus the first branch of every choice.
function baseArgs(spec) {
  const args = {};
  for (const p of spec.params) if (p.required) args[p.name] = sampleParam(p);
  for (const c of spec.choices ?? []) for (const n of c.branches[0]) args[n] = sampleParam(spec.params.find((p) => p.name === n));
  return args;
}
// The JSON kinds a parameter accepts; a step sends a value of none of them.
const KIND = { string: "string", number: "number", integer: "number", boolean: "boolean", object: "object", vec3: "object", rotator: "object", color: "object", array: "array" };
const FORM_KIND = { argMap: "object", argEntryList: "array", stringList: "array", string: "string" };
function wrongValue(p) {
  if (p.type === "any" && !p.forms?.length) return undefined;
  const kinds = new Set(p.forms?.length ? p.forms.map((f) => FORM_KIND[f]) : [KIND[p.type], ...(p.orTypes ?? []).map((t) => KIND[t])]);
  const candidates = [["boolean", true], ["number", 12345], ["string", "not-a-valid-value"], ["array", [1]], ["object", { unexpectedField: 1 }]];
  return candidates.find(([kind]) => !kinds.has(kind))?.[1];
}
const below = (n, integer) => (n > -1e15 ? n - (integer ? 1 : Math.max(1, Math.abs(n))) : n * 2);
const above = (n, integer) => (n < 1e15 ? n + (integer ? 1 : Math.max(1, Math.abs(n))) : n * 2);

const invalid = (method, name) => new RegExp(`^Invalid parameters for ${method}: .*\\b${name}\\b`);
for (const [method, spec] of Object.entries(specs)) {
  if (!manifestHandlers[method]) continue;
  const base = baseArgs(spec);
  const call = async (label, args, pattern) => step(`contract ${method} ${label}`, method, args, undefined, { expectError: pattern });
  await call("unknown key", { ...base, unexpectedKey: 1 }, invalid(method, "unexpectedKey"));
  for (const p of spec.params) {
    const n = p.name;
    const wrong = wrongValue(p);
    if (wrong !== undefined) await call(`${n} wrong type`, { ...base, [n]: wrong }, invalid(method, n));
    const isInt = p.type === "integer" || p.items === "integer";
    const wrap = (v) => (p.type === "array" ? [v] : v);
    if (isInt) await call(`${n} not an integer`, { ...base, [n]: wrap((p.minimum ?? 0) + 0.5) }, invalid(method, n));
    if (p.minimum !== undefined) await call(`${n} below minimum`, { ...base, [n]: wrap(below(p.minimum, isInt)) }, invalid(method, n));
    if (p.maximum !== undefined) await call(`${n} above maximum`, { ...base, [n]: wrap(above(p.maximum, isInt)) }, invalid(method, n));
    if (p.enum?.length) await call(`${n} bad enum value`, { ...base, [n]: wrap("NotAnEnumValue") }, invalid(method, n));
    if (p.literal !== undefined) await call(`${n} not the literal`, { ...base, [n]: "NotTheLiteral" }, invalid(method, n));
    if (p.oneOf) await call(`${n} bad ${p.oneOf.key}`, { ...base, [n]: wrap({ [p.oneOf.key]: "NotAVariant" }) }, invalid(method, n));
    if (p.required) {
      const { [n]: _, ...rest } = base;
      await call(`${n} missing`, rest, invalid(method, `needs ${n}`));
    }
    if (!p.nullable && p.type !== "any") await call(`${n} null`, { ...base, [n]: null }, invalid(method, n));
  }
  for (const c of spec.choices ?? []) {
    const names = c.branches.flat();
    const without = Object.fromEntries(Object.entries(base).filter(([k]) => !names.includes(k)));
    await call(`${c.mode} ${names.slice(0, 3).join("/")} none given`, without, invalid(method, "needs"));
    if (c.mode === "exactlyOne") {
      const both = { ...without };
      for (const b of c.branches.slice(0, 2)) for (const n of b) both[n] = sampleParam(spec.params.find((p) => p.name === n));
      await call(`${c.mode} ${names.slice(0, 2).join("/")} both given`, both, invalid(method, "takes one side"));
    }
  }
}

// Assets
for (const [type, name] of [
  ["height_graph", "HG_Smoke"], ["surface_type", "ST_Grass"], ["surface_type", "ST_Dirt"], ["mega_material", "MM_Smoke"],
  ["layer_stack", "LS_Smoke"], ["height_layer", "HL_Smoke"], ["smart_surface_type", "SST_Smoke"],
  ["smart_surface_type_graph", "SSG_Smoke"], ["height_sculpt_asset", "HSA_Smoke"], ["pcg_graph", "PG_Smoke"],
]) {
  await step(`asset_create ${type}`, "voxel_asset_create", { type, name, packagePath: P }, (r) => !!r.assetPath || "no assetPath");
}
await step("asset_create skip existing", "voxel_asset_create", { type: "surface_type", name: "ST_Grass", packagePath: P }, (r) => r.existed === true || "expected existed");
await step("graph has output node", "voxel_graph_read", { assetPath: `${P}/HG_Smoke`, includePins: false }, (r) =>
  r.nodes.some((n) => /Output Height/.test(n.title)) || "factory did not duplicate the template");

await step("surface_type_set", "voxel_surface_type_set", { assetPath: `${P}/ST_Grass`, material: "/Engine/BasicShapes/BasicShapeMaterial" });
await step("mega_material_set_surfaces", "voxel_mega_material_set_surfaces", { assetPath: `${P}/MM_Smoke`, surfaceTypes: [`${P}/ST_Grass`, `${P}/ST_Dirt`] });
await step("layer_stack_set", "voxel_layer_stack_set", { assetPath: `${P}/LS_Smoke`, heightLayers: [`${P}/HL_Smoke`] });
await step("smart_surface_set graph", "voxel_smart_surface_set", { assetPath: `${P}/SST_Smoke`, graph: `${P}/SSG_Smoke` });
await step("asset_set_property", "voxel_asset_set_property", { assetPath: `${P}/ST_Dirt`, propertyName: "BlendSmoothness", value: "0.3" });

// Graph: Amplitude parameter wired into the template's noise
await step("graph_add_parameter", "voxel_graph_add_parameter", { assetPath: `${P}/HG_Smoke`, name: "Amplitude", type: "float", default: "2000" });
const getter = await step("graph_add_node getter", "voxel_graph_add_node", { assetPath: `${P}/HG_Smoke`, nodeType: "Parameters|Amplitude", x: -300, y: 300 });
await step("graph_connect", "voxel_graph_connect", () => ({
  assetPath: `${P}/HG_Smoke`, fromNode: getter.node?.id, fromPin: "Value", toNode: "Advanced Noise 2D", toPin: "Amplitude",
}));

// World and stamp
const world = await step("world_spawn", "voxel_world_spawn", { label: "SmokeWorld", megaMaterial: `${P}/MM_Smoke`, voxelSize: 100 }, (r) => !!r.actorPath || "no actorPath");
ctx.world = world.actorPath;
await step("world_configure", "voxel_world_configure", () => ({ actorPath: ctx.world, enableNanite: false, renderChunkSize: "Size64" }));
await step("world_configure bad enum", "voxel_world_configure", () => ({ actorPath: ctx.world, renderChunkSize: "Size999" }), undefined, { expectError: true });
const stamp = await step("actor_spawn stamp", "voxel_actor_spawn", { kind: "stamp", label: "SmokeStamp" }, (r) => !!r.actorPath || "no actorPath");
ctx.stamp = stamp.actorPath;
await step("stamp_set height_graph", "voxel_stamp_set", () => ({
  actorPath: ctx.stamp, kind: "height_graph", asset: `${P}/HG_Smoke`, blendMode: "Override", parameters: { Amplitude: 3500 },
}));
await step("stamp_read", "voxel_stamp_read", () => ({ actorPath: ctx.stamp }), (r) =>
  (r.stamp?.kind === "height_graph" && /HG_Smoke/.test(r.stamp.asset ?? "") && !!r.stamp.layer && r.stamp.overrides?.Amplitude?.startsWith("3500")) || `unexpected ${JSON.stringify(r.stamp)}`);
await step("stamp_set_parameters", "voxel_stamp_set_parameters", () => ({ actorPath: ctx.stamp, values: { Amplitude: "4000" } }));
// Object parameters: a bare package path must resolve, an unresolved one must be refused, never dropped silently
await step("graph_add_parameter surface", "voxel_graph_add_parameter", { assetPath: `${P}/HG_Smoke`, name: "Surface", type: "struct:/Script/Voxel.VoxelSurfaceType" });
await step("stamp_set_parameters package path", "voxel_stamp_set_parameters", () => ({ actorPath: ctx.stamp, values: { Surface: `${P}/ST_Dirt` } }), (r) =>
  /ST_Dirt\.ST_Dirt$/.test(r.overrides?.Surface ?? "") || `override ${JSON.stringify(r.overrides)}`);
await step("stamp_set_parameters unresolved object", "voxel_stamp_set_parameters", () => ({ actorPath: ctx.stamp, values: { Surface: `${P}/ST_Missing` } }), undefined, { expectError: true });
await step("world_status", "voxel_world_status", () => ({ actorPath: ctx.world }), (r) => typeof r.progress === "number" || "no progress");
await step("query_layer", "voxel_query_layer", { points: [{ x: 0, y: 0 }, { x: 5000, y: 5000 }], querySurface: true }, (r) =>
  (r.points?.length === 2 && typeof r.points[0].height === "number") || `points ${JSON.stringify(r.points)}`);

// Instanced stamps and no-clipping on a second actor
const holder = await step("actor_spawn stamp holder", "voxel_actor_spawn", { kind: "stamp", label: "SmokeInstances", location: { x: 20000, y: 0, z: 0 } });
ctx.holder = holder.actorPath;
await step("component_add instanced_stamp", "voxel_component_add", () => ({ actorPath: ctx.holder, kind: "instanced_stamp", componentName: "Instances" }));
await step("instanced_stamps add", "voxel_instanced_stamps", () => ({
  actorPath: ctx.holder, componentName: "Instances", op: "add", kind: "height_graph", asset: `${P}/HG_Smoke`,
  transforms: [{ location: { x: 20000, y: 0, z: 0 } }, { location: { x: 24000, y: 0, z: 0 } }],
}), (r) => r.count === 2 || `count ${r.count}`);
await step("instanced_stamps remove", "voxel_instanced_stamps", () => ({ actorPath: ctx.holder, componentName: "Instances", op: "remove", index: 0 }), (r) => r.validCount === 1 || `validCount ${r.validCount}`);
await step("instanced_stamps clear", "voxel_instanced_stamps", () => ({ actorPath: ctx.holder, componentName: "Instances", op: "clear" }), (r) => r.validCount === 0 || `validCount ${r.validCount}`);
await step("component_add no_clipping", "voxel_component_add", () => ({ actorPath: ctx.holder, kind: "no_clipping", componentName: "NoClip" }));
await step("no_clipping_set_layer", "voxel_no_clipping_set_layer", () => ({ actorPath: ctx.holder, componentName: "NoClip", autoAdjustPlayer: true }));

// Sculpting
const hs = await step("actor_spawn height_sculpt", "voxel_actor_spawn", { kind: "height_sculpt", label: "SmokeHeightSculpt" });
ctx.hs = hs.actorPath;
await step("height_sculpt sculpt_height", "voxel_height_sculpt", () => ({ actorPath: ctx.hs, op: "sculpt_height", center: { x: 0, y: 0, z: 0 }, radius: 2000, strength: 0.5 }));
await step("height_sculpt smooth", "voxel_height_sculpt", () => ({ actorPath: ctx.hs, op: "smooth", center: { x: 0, y: 0, z: 0 }, radius: 2000, strength: 0.5 }));
await step("sculpt_asset_set", "voxel_sculpt_asset_set", () => ({ actorPath: ctx.hs, asset: `${P}/HSA_Smoke`, load: false }));
await step("sculpt_asset_get", "voxel_sculpt_asset_get", () => ({ actorPath: ctx.hs }), (r) => /HSA_Smoke/.test(r.asset ?? "") || `asset ${r.asset}`);
await step("height_sculpt clear_data", "voxel_height_sculpt", () => ({ actorPath: ctx.hs, op: "clear_data" }));
const vs = await step("actor_spawn volume_sculpt", "voxel_actor_spawn", { kind: "volume_sculpt", label: "SmokeVolumeSculpt" });
ctx.vs = vs.actorPath;
await step("volume_sculpt sphere", "voxel_volume_sculpt", () => ({ actorPath: ctx.vs, op: "sphere", center: { x: 0, y: 0, z: 0 }, radius: 1500 }));

// PCG: an engine PCG graph (ue-mcp core creates it) dressed with voxel nodes
await step("create engine PCG graph", "create_pcg_graph", { name: "PG_Engine", packagePath: P });
const node = await step("pcg_add_node sampler_v2", "voxel_pcg_add_node", { graphPath: `${P}/PG_Engine`, nodeType: "sampler_v2" }, (r) => !!r.name || "no node name");
await step("pcg_configure_sampler", "voxel_pcg_configure_sampler", () => ({ graphPath: `${P}/PG_Engine`, node: node.name, distanceBetweenPoints: 400, resolveSmartSurfaceTypes: true }));

// Runtime
await step("world_runtime destroy", "voxel_world_runtime", () => ({ actorPath: ctx.world, op: "destroy" }));
await step("world_runtime create", "voxel_world_runtime", () => ({ actorPath: ctx.world, op: "create" }));

// Regression: a saved graph edit must survive a reload (the compiled graph is flushed before saving)
await step("asset_create reload graph", "voxel_asset_create", { type: "height_graph", name: "HG_Reload", packagePath: P });
// Amplitude 0 flattens the stamp to height 0; a stale compiled graph keeps the noise.
const far = await step("actor_spawn reload stamp", "voxel_actor_spawn", { kind: "stamp", label: "SmokeReload", location: { x: 60000, y: 0, z: 0 } });
await step("stamp_set reload graph", "voxel_stamp_set", () => ({ actorPath: far.actorPath, kind: "height_graph", asset: `${P}/HG_Reload` }));
await step("reload stamp has height", "voxel_query_layer", { points: [{ x: 60000, y: 0 }] }, (r) => typeof r.points?.[0]?.height === "number" || `height ${r.points?.[0]?.height}`);
await step("graph edit saved", "voxel_graph_set_pin_default", { assetPath: `${P}/HG_Reload`, node: "Advanced Noise 2D", pin: "Amplitude", value: "0" });
await step("reload graph from disk", "force_reload_asset", { assetPath: `${P}/HG_Reload` });
await step("runtime rebuilt", "voxel_world_runtime", () => ({ actorPath: ctx.world, op: "destroy" }));
await step("runtime recreated", "voxel_world_runtime", () => ({ actorPath: ctx.world, op: "create" }));
await step("reloaded graph keeps the edit", "voxel_query_layer", { points: [{ x: 60000, y: 0 }] }, (r) => r.points?.[0]?.height === 0 || `stale compiled graph: height ${r.points?.[0]?.height}`);

// Edge cases from review
await step("world_configure accepts save:false", "voxel_world_configure", () => ({ actorPath: ctx.world, enableLumen: true, save: false }));
await step("asset_set_property refuses a level actor", "voxel_asset_set_property", () => ({ assetPath: ctx.world, propertyName: "VoxelSize", value: "50" }), undefined, { expectError: true });
const doomed = await step("actor_spawn to delete", "voxel_actor_spawn", { kind: "stamp", label: "SmokeDoomed" });
await step("delete actor", "delete_actor", () => ({ actorPath: doomed.actorPath }));
await step("deleted actor path refused", "voxel_stamp_read", () => ({ actorPath: doomed.actorPath }), undefined, { expectError: true });

// Input a handler used to coerce, ignore or default silently is refused, by the contract or by the handler itself.
const refuse = (name, method, params, pattern) => step(name, method, params, undefined, { expectError: pattern });
const center = { x: 0, y: 0, z: 0 };
await refuse("stamp_set refuses a volume-only field on a height kind", "voxel_stamp_set",
  () => ({ actorPath: ctx.stamp, kind: "height_graph", boundsExtensionMultiplier: 1 }), /boundsExtensionMultiplier only applies to volume/);
await refuse("stamp_set refuses an empty asset", "voxel_stamp_set", () => ({ actorPath: ctx.stamp, kind: "height_graph", asset: "" }), /asset must not be empty/);
await refuse("stamp_set refuses an empty layer", "voxel_stamp_set", () => ({ actorPath: ctx.stamp, kind: "height_graph", layer: "" }), /layer must not be empty/);
await refuse("stamp_set refuses an object parameter value", "voxel_stamp_set",
  () => ({ actorPath: ctx.stamp, kind: "height_graph", parameters: { Amplitude: { value: 1 } } }), /parameters\.Amplitude must be a string, number, boolean or null/);
await refuse("stamp_read refuses an empty componentName", "voxel_stamp_read", () => ({ actorPath: ctx.stamp, componentName: "" }), /componentName must not be empty/);
await refuse("stamp_read refuses actorPath and actorLabel together", "voxel_stamp_read", () => ({ actorPath: ctx.stamp, actorLabel: "SmokeStamp" }), /takes one side/);
await refuse("graph_add_parameter refuses a fractional int default", "voxel_graph_add_parameter",
  { assetPath: `${P}/HG_Smoke`, name: "Count", type: "int32", default: 2.5 }, /does not parse as int/i);
await refuse("instanced_stamps count refuses index", "voxel_instanced_stamps",
  () => ({ actorPath: ctx.holder, componentName: "Instances", op: "count", index: 0 }), /index does not apply to op count/);
await refuse("instanced_stamps update refuses empty indices", "voxel_instanced_stamps",
  () => ({ actorPath: ctx.holder, componentName: "Instances", op: "update", indices: [] }), /indices must not be empty/);
await refuse("instanced_stamps add refuses a partial rotation", "voxel_instanced_stamps", () => ({
  actorPath: ctx.holder, componentName: "Instances", op: "add", kind: "height_graph", asset: `${P}/HG_Smoke`,
  transforms: [{ location: center, rotation: { yaw: 90 } }],
}), /transforms\[0\]\.rotation needs pitch/);
await refuse("instanced_stamps add refuses a string scale", "voxel_instanced_stamps", () => ({
  actorPath: ctx.holder, componentName: "Instances", op: "add", kind: "height_graph", asset: `${P}/HG_Smoke`,
  transforms: [{ location: center, scale: "2" }],
}), /transforms\[0\]\.scale must be a number or \{x,y,z\}/);
await refuse("height_sculpt smooth refuses mode", "voxel_height_sculpt", () => ({ actorPath: ctx.hs, op: "smooth", center, mode: "Add" }), /mode does not apply to op smooth/);
await refuse("height_sculpt clear_cache refuses center", "voxel_height_sculpt", () => ({ actorPath: ctx.hs, op: "clear_cache", center }), /center does not apply to op clear_cache/);
await refuse("height_sculpt refuses two brush falloffs", "voxel_height_sculpt",
  () => ({ actorPath: ctx.hs, op: "sculpt_height", center, falloff: 0.5, brush: { type: "Circular", falloffAmount: 0.2 } }), /both set the brush falloff/);
await refuse("height_sculpt refuses a brush without a type", "voxel_height_sculpt",
  () => ({ actorPath: ctx.hs, op: "sculpt_height", center, brush: { falloffAmount: 0.2 } }), /brush\.type must be one of/);
await refuse("height_sculpt refuses an Alpha field on a Circular brush", "voxel_height_sculpt",
  () => ({ actorPath: ctx.hs, op: "sculpt_height", center, brush: { type: "Circular", autoRotate: false } }), /brush\.autoRotate is not a field/);
await refuse("volume_sculpt cube refuses radius", "voxel_volume_sculpt", () => ({ actorPath: ctx.vs, op: "cube", center, radius: 100 }), /radius does not apply to op cube/);
await refuse("volume_sculpt refuses a string coordinate", "voxel_volume_sculpt", () => ({ actorPath: ctx.vs, op: "sphere", center: { x: "0", y: 0, z: 0 } }), /center\.x must be a number/);
await refuse("sculpt_asset_set refuses load when detaching", "voxel_sculpt_asset_set", () => ({ actorPath: ctx.hs, asset: "", load: true }), /load only applies when binding/);
await refuse("query_layer refuses z on a height layer", "voxel_query_layer", { points: [{ x: 0, y: 0, z: 0 }] }, /points\[0\] must be \{x,y\} for a height layer/);
await refuse("query_layer refuses an empty stack", "voxel_query_layer", { stack: "", points: [{ x: 0, y: 0 }] }, /stack must not be empty/);
await refuse("world_configure refuses a string interval bound", "voxel_world_configure",
  () => ({ actorPath: ctx.world, lodQuality: { gameQuality: { min: "1" } } }), /lodQuality\.gameQuality\.min must be a number/);
await refuse("world_configure refuses a string boolean", "voxel_world_configure",
  () => ({ actorPath: ctx.world, lodQuality: { alwaysUseGameQuality: "yes" } }), /lodQuality\.alwaysUseGameQuality must be a boolean/);
await refuse("world_configure refuses a display-name enum spelling", "voxel_world_configure", () => ({ actorPath: ctx.world, renderChunkSize: "64" }), /renderChunkSize must be one of/);
await refuse("mega_material_set_surfaces refuses mode without surfaceTypes", "voxel_mega_material_set_surfaces",
  { assetPath: `${P}/MM_Smoke`, mode: "append", enableSmoothBlends: true }, /mode only applies with surfaceTypes/);
await refuse("mega_material_set_surfaces refuses a repeated surface type", "voxel_mega_material_set_surfaces",
  { assetPath: `${P}/MM_Smoke`, surfaceTypes: [`${P}/ST_Grass`, `${P}/ST_Grass`] }, /listed twice/);
await refuse("layer_stack_set refuses a repeated layer", "voxel_layer_stack_set", { assetPath: `${P}/LS_Smoke`, heightLayers: [`${P}/HL_Smoke`, `${P}/HL_Smoke`] }, /listed twice/);
await refuse("asset_set_property refuses an enum index", "voxel_asset_set_property",
  { assetPath: `${P}/MM_Smoke`, propertyName: "LumenMaterialType", value: "1" }, /is not a EVoxelMegaMaterialGenerationType value/);
await refuse("asset_set_property refuses an unloadable soft reference", "voxel_asset_set_property",
  { assetPath: `${P}/MM_Smoke`, propertyName: "DitherNoiseTexture", value: `${P}/T_Missing` }, /No Texture2D at/);
await refuse("graph_export_t3d refuses an empty node list", "voxel_graph_export_t3d", { assetPath: `${P}/HG_Smoke`, nodes: [] }, /nodes must be a non-empty array/);
await refuse("graph_read refuses an empty terminalGraph", "voxel_graph_read", { assetPath: `${P}/HG_Smoke`, terminalGraph: "" }, /terminalGraph must not be empty/);
await refuse("graph_add_parameter refuses an empty default", "voxel_graph_add_parameter",
  { assetPath: `${P}/HG_Smoke`, name: "Empty", type: "float", default: "" }, /default must be a non-empty/);
await refuse("graph_set_pin_default refuses a null value", "voxel_graph_set_pin_default",
  { assetPath: `${P}/HG_Smoke`, node: "Advanced Noise 2D", pin: "Amplitude", value: null }, /value must not be null/);
await refuse("stamp_set_parameters refuses an array value", "voxel_stamp_set_parameters",
  () => ({ actorPath: ctx.stamp, values: { Amplitude: [1] } }), /values\.Amplitude must be a string, number, boolean or null/);
await refuse("pcg_add_node refuses a fractional position", "voxel_pcg_add_node", { graphPath: `${P}/PG_Engine`, nodeType: "query", x: 10.5 }, /x must be an integer/);
await refuse("pcg_configure_sampler refuses a fractional lod", "voxel_pcg_configure_sampler", () => ({ graphPath: `${P}/PG_Engine`, node: node.name, lod: 2.5 }), /lod must be an integer/);

// Every asset edit saves itself; only the level may be left dirty.
await step("no unsaved assets", "list_dirty_packages", {}, (r) => {
  const dirty = (r.content ?? []).map((c) => c.package);
  return dirty.length === 0 || `unsaved: ${dirty.join(", ")}`;
});

console.log(`\n${results.length - failed}/${results.length} passed`);
process.exit(failed ? 1 : 0);
