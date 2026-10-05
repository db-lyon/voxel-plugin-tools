// Manifest <-> native handler consistency. No editor needed: node scripts/check.mjs
import { readFileSync, readdirSync, existsSync } from "node:fs";
import { dirname, resolve, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import yaml from "js-yaml";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const manifest = yaml.load(readFileSync(resolve(root, "ue-mcp.plugin.yml"), "utf8"));
const errors = [];
const ok = (m) => console.log(`  ok  ${m}`);

// Names the gateway or dispatcher consumes, or that collide with built-in params.
const RESERVED = new Set(["action", "editor", "select", "omit", "timeoutMs", "position"]);

// C0: the host's own schema, the same one the server loads with.
const hostManifest = resolve(root, "node_modules/ue-mcp/dist/extensions/manifest.js");
if (!existsSync(hostManifest)) {
  errors.push(`C0: ${hostManifest} missing; run npm install`);
} else {
  const { PluginManifestSchema } = await import(pathToFileURL(hostManifest).href);
  const r = PluginManifestSchema.safeParse(manifest);
  if (r.success) ok("C0 manifest validates against the host schema");
  else for (const i of r.error.issues) errors.push(`C0: ${i.path.join(".")}: ${i.message}`);
}

// C1: manifest handlers and C++ registrations are the same set.
const handlers = manifest.nativeModule?.handlers ?? {};
const srcDir = resolve(root, manifest.nativeModule.source, "Source/VoxelPluginTools/Private");
const registered = new Set();
for (const file of readdirSync(srcDir).filter((f) => f.endsWith(".cpp"))) {
  const text = readFileSync(join(srcDir, file), "utf8");
  for (const m of text.matchAll(/\{\s*TEXT\("(voxel_[a-z0-9_]+)"\)\s*,/g)) registered.add(m[1]);
}
for (const name of registered) if (!handlers[name]) errors.push(`C1: ${name} is registered in C++ but missing from the manifest`);
for (const name of Object.keys(handlers)) if (!registered.has(name)) errors.push(`C1: ${name} is in the manifest but not registered in C++`);
ok(`C1 ${registered.size} C++ handlers, ${Object.keys(handlers).length} manifest handlers`);

// C2: every handler declares its effect, documents its params, and avoids reserved names.
for (const [name, spec] of Object.entries(handlers)) {
  if (!["read", "mutate"].includes(spec.effect)) errors.push(`C2: ${name}: effect must be read or mutate`);
  const desc = spec.description ?? "";
  const at = desc.search(/\bParams:/);
  if (at < 0) {
    errors.push(`C2: ${name}: description has no Params: clause`);
    continue;
  }
  const clause = desc.slice(at + "Params:".length);
  for (const [field, f] of Object.entries(spec.schema ?? {})) {
    if (RESERVED.has(field)) errors.push(`C2: ${name}.${field} is a reserved or colliding parameter name`);
    if (!f?.description) errors.push(`C2: ${name}.${field} has no description`);
    if (!new RegExp(`\\b${field}\\b`).test(clause)) errors.push(`C2: ${name}.${field} is missing from the Params: clause`);
  }
}
ok("C2 effects, descriptions and Params clauses checked");

// C3: flows reference native actions that exist.
for (const [flow, spec] of Object.entries(manifest.flows ?? {})) {
  for (const [step, s] of Object.entries(spec.steps ?? {})) {
    const m = /^voxel\.(voxel_[a-z0-9_]+)$/.exec(s.task ?? "");
    if (m && !handlers[m[1]]) errors.push(`C3: flow ${flow} step ${step} calls unknown action ${s.task}`);
  }
}
ok("C3 flow steps resolve");

// C4: the module's read-only set (no save bookkeeping) matches the manifest's effect: read.
const moduleSrc = readFileSync(join(srcDir, "VoxelPluginToolsModule.cpp"), "utf8");
const readBlock = /ReadHandlers =\s*\{([\s\S]*?)\};/.exec(moduleSrc)?.[1] ?? "";
const moduleRead = new Set([...readBlock.matchAll(/TEXT\("(voxel_[a-z0-9_]+)"\)/g)].map((m) => m[1]));
const manifestRead = new Set(Object.entries(handlers).filter(([, s]) => s.effect === "read").map(([n]) => n));
for (const n of manifestRead) if (!moduleRead.has(n)) errors.push(`C4: ${n} is effect: read but missing from ReadHandlers`);
for (const n of moduleRead) if (!manifestRead.has(n)) errors.push(`C4: ${n} is in ReadHandlers but not effect: read`);
ok(`C4 ${moduleRead.size} read-only handlers agree`);

if (errors.length) {
  for (const e of errors) console.error(`  ERR ${e}`);
  console.error(`errors: ${errors.length}`);
  process.exit(1);
}
console.log("errors: 0");
