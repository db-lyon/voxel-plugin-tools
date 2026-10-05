// Live smoke test: drives every native handler against a running editor and asserts the results.
// node scripts/live-test.mjs [path/to/project.uproject]   (default: the test project; open an empty map first)
import path from "node:path";
import { fileURLToPath } from "node:url";

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
    const ok = reply?.success === false;
    if (!ok) failed++;
    results.push({ name, ok, why: ok ? "" : "expected an error" });
    console.log(`${ok ? "PASS" : "FAIL"}  ${name}${ok ? "" : "  -> expected an error"}`);
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

// Every asset edit saves itself; only the level may be left dirty.
await step("no unsaved assets", "list_dirty_packages", {}, (r) => {
  const dirty = (r.content ?? []).map((c) => c.package);
  return dirty.length === 0 || `unsaved: ${dirty.join(", ")}`;
});

console.log(`\n${results.length - failed}/${results.length} passed`);
process.exit(failed ? 1 : 0);
