// Static manifest <-> task consistency check (TODO Section C).
// No editor needed. Run: node scripts/static-check.mjs
import { readFileSync, existsSync } from "node:fs";
import { pathToFileURL } from "node:url";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import yaml from "js-yaml";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const manifest = yaml.load(readFileSync(resolve(root, "ue-mcp.plugin.yml"), "utf8"));

const errors = [];
const warnings = [];
const ok = (m) => console.log(`  ok  ${m}`);

// ── C0: the manifest validates against the HOST's own zod schema ──
// This is what the server runs at load time; a failure here means the plugin is
// silently skipped (e.g. a schema field missing its required `type`). Validate
// here so we catch it before a wasted reconnect.
console.log("[C0] manifest validates against the host PluginManifestSchema");
try {
  // Import by absolute file path — ue-mcp's package "exports" map doesn't expose
  // this internal module as a subpath, but the file is there and is the schema
  // the server actually loads with.
  const manifestJs = resolve(root, "node_modules/ue-mcp/dist/plugin/manifest.js");
  const { PluginManifestSchema } = await import(pathToFileURL(manifestJs).href);
  const r = PluginManifestSchema.safeParse(manifest);
  if (!r.success) {
    for (const issue of r.error.issues) {
      errors.push(`C0: ${issue.path.join(".")}: ${issue.message} (${issue.code})`);
    }
  } else {
    ok("manifest is valid per the host schema");
  }
} catch (e) {
  warnings.push(`C0: could not load host PluginManifestSchema (${e.message}); skipped — host-side load is the real gate`);
}

// Collect injected task refs: inject.<category>.<action>.task
const injectedTasks = new Set();
const injectActionToTask = {};
for (const [cat, actions] of Object.entries(manifest.inject ?? {})) {
  for (const [action, def] of Object.entries(actions)) {
    if (!def || !def.task) {
      errors.push(`inject.${cat}.${action} has no 'task' key`);
      continue;
    }
    injectedTasks.add(def.task);
    injectActionToTask[`${cat}.${action}`] = def.task;
  }
}

// Tasks table
const tasks = manifest.tasks ?? {};
const taskKeys = new Set(Object.keys(tasks));

// Flow task refs (only voxel.* are ours; host tasks like level.* are external)
const flowTaskRefs = new Set();
for (const [fname, fdef] of Object.entries(manifest.flows ?? {})) {
  for (const [sidx, step] of Object.entries(fdef.steps ?? {})) {
    if (step?.task) flowTaskRefs.add(step.task);
  }
}

// ── C1: every injected task ref is a defined tasks: entry ──
console.log("\n[C1] injected task refs resolve to a tasks: entry");
for (const [action, t] of Object.entries(injectActionToTask)) {
  if (!taskKeys.has(t)) errors.push(`C1: inject ${action} -> task '${t}' not defined in tasks:`);
}
for (const t of flowTaskRefs) {
  if (t.startsWith("voxel.") && !taskKeys.has(t)) {
    errors.push(`C1: flow step -> task '${t}' not defined in tasks:`);
  }
}
if (!errors.some((e) => e.startsWith("C1"))) ok(`${Object.keys(injectActionToTask).length} injected refs all resolve`);

// ── C4: no orphan tasks (defined but never injected, and not a flow-only ref) ──
console.log("\n[C4] no orphan tasks (defined but not injected)");
for (const t of taskKeys) {
  if (!injectedTasks.has(t) && !flowTaskRefs.has(t)) {
    warnings.push(`C4: task '${t}' defined but never injected or used in a flow`);
  }
}
if (!warnings.some((w) => w.startsWith("C4"))) ok(`all ${taskKeys.size} tasks are injected or flow-referenced`);

// ── C5: no injected field uses `required: true` ──
// The host lifts every action's schema fields to ONE flat schema per category
// ("action selects which params apply"), and compileSchemaFields keeps a
// `required` field non-optional — so it becomes required for the WHOLE category
// tool (every sibling action, built-ins included). Requiredness must be enforced
// by each task's validate() at runtime, not in the manifest.
console.log("\n[C5] no injected schema field is marked required:true");
let c5 = 0;
for (const [cat, actions] of Object.entries(manifest.inject ?? {})) {
  for (const [action, def] of Object.entries(actions)) {
    for (const [f, spec] of Object.entries(def.schema ?? {})) {
      if (spec && spec.required === true) errors.push(`C5: inject.${cat}.${action}.${f} is required:true — makes '${f}' required for the entire '${cat}' tool`);
      else c5++;
    }
  }
}
if (!errors.some((e) => e.startsWith("C5"))) ok(`${c5} injected fields, none required:true`);

// ── C6: within a category, a reused field name must keep one type ──
// Fields merge by name into the flat category schema (first-wins). A name used
// with two types across actions means one action's calls validate against the
// wrong type. (Benign only when a built-in category field of compatible type
// absorbs it — flagged as a warning to review, e.g. `value` on `level`.)
console.log("\n[C6] no within-category field-name type collisions");
for (const [cat, actions] of Object.entries(manifest.inject ?? {})) {
  const types = {}; // field -> Set(type)
  for (const def of Object.values(actions)) {
    for (const [f, spec] of Object.entries(def.schema ?? {})) {
      (types[f] ??= new Set()).add(spec?.type ?? "(none)");
    }
  }
  for (const [f, set] of Object.entries(types)) {
    if (set.size > 1) warnings.push(`C6: inject.${cat} field '${f}' has multiple types (${[...set].join("/")}) across actions — fine only if a built-in '${f}' of compatible type absorbs it`);
  }
}
if (!warnings.some((w) => w.startsWith("C6"))) ok(`no field-name type collisions in any category`);

// ── C2: class_path resolves to a built dist file whose default export's taskName matches the key ──
console.log("\n[C2] class_path -> dist file -> default export taskName matches key");
let c2checked = 0;
for (const [key, def] of Object.entries(tasks)) {
  const cp = def.class_path;
  if (!cp) { errors.push(`C2: task '${key}' has no class_path`); continue; }
  const distJs = resolve(root, "dist", `${cp}.js`);
  if (!existsSync(distJs)) {
    errors.push(`C2: task '${key}' class_path '${cp}' -> missing dist file ${cp}.js`);
    continue;
  }
  try {
    const mod = await import(pathToFileURL(distJs).href);
    const Cls = mod.default;
    if (typeof Cls !== "function") {
      errors.push(`C2: ${cp}.js has no default-exported class`);
      continue;
    }
    // taskName is an instance getter; instantiate with a stub.
    let inst;
    try { inst = new Cls({}, {}); } catch { try { inst = new Cls(); } catch (e2) { inst = null; } }
    const tn = inst && inst.taskName;
    if (tn == null) {
      warnings.push(`C2: ${cp}.js -> could not read taskName (instantiation needs args); skipped name match`);
    } else if (tn !== key) {
      errors.push(`C2: ${cp}.js taskName '${tn}' != tasks key '${key}'`);
    } else {
      c2checked++;
    }
  } catch (e) {
    errors.push(`C2: import ${cp}.js failed: ${e.message}`);
  }
}
ok(`${c2checked}/${Object.keys(tasks).length} tasks: dist present + taskName matches`);

// ── Summary ──
console.log("\n──────── SUMMARY ────────");
console.log(`injected actions: ${Object.keys(injectActionToTask).length}`);
console.log(`tasks defined:    ${taskKeys.size}`);
console.log(`flows:            ${Object.keys(manifest.flows ?? {}).length}`);
console.log(`warnings: ${warnings.length}, errors: ${errors.length}`);
for (const w of warnings) console.log(`  WARN  ${w}`);
for (const e of errors) console.log(`  ERR   ${e}`);
process.exit(errors.length ? 1 : 0);
