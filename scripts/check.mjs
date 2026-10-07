// Manifest <-> native handler consistency. No editor needed: node scripts/check.mjs
import { readFileSync, readdirSync, existsSync } from "node:fs";
import { dirname, resolve, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import yaml from "js-yaml";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const manifest = yaml.load(readFileSync(resolve(root, "ue-mcp.plugin.yml"), "utf8"));
const pkg = JSON.parse(readFileSync(resolve(root, "package.json"), "utf8"));
const errors = [];
const ok = (m) => console.log(`  ok  ${m}`);

// Names the dispatcher consumes before a handler runs (ue-mcp src/surface/routing-params.ts); the bridge refuses a
// contract that declares one.
const ROUTING = new Set(["action", "timeoutMs", "select", "omit", "editor", "toEditor"]);

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

// C1: the contracts come from C++ (bridge ABI 2), recorded into the file the manifest names and shipped with it.
const native = manifest.nativeModule ?? {};
if (!(native.minBridgeApi >= 2)) errors.push(`C1: nativeModule.minBridgeApi is ${native.minBridgeApi}; typed contracts need 2`);
if (!native.specs) errors.push("C1: nativeModule.specs is not set; the server would surface no contract");
else if (!(pkg.files ?? []).includes(native.specs)) errors.push(`C1: package.json files does not ship ${native.specs}`);
ok(`C1 minBridgeApi ${native.minBridgeApi}, specs ${native.specs}`);

// C2: every C++ registration, parsed from the Add*Handlers bodies: Out.Add({ TEXT("name"), fn, { params }, rules? }).
const srcDir = resolve(root, native.source, "Source/VoxelPluginTools/Private");
const sources = Object.fromEntries(readdirSync(srcDir).filter((f) => /\.(cpp|h)$/.test(f)).map((f) => [f, readFileSync(join(srcDir, f), "utf8")]));

// The source with comments, strings and char literals blanked to spaces, so braces and commas inside them are ignored.
function code(text) {
  return text.replace(/\/\/[^\n]*|\/\*[\s\S]*?\*\/|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'/g, (m) => " ".repeat(m.length));
}

// Index of the bracket that closes the one at `open`.
function closing(masked, open) {
  const pairs = { "(": ")", "{": "}", "[": "]" };
  const stack = [];
  for (let i = open; i < masked.length; i++) {
    const c = masked[i];
    if (pairs[c]) stack.push(pairs[c]);
    else if (c === ")" || c === "}" || c === "]") {
      if (stack.pop() !== c) return -1;
      if (stack.length === 0) return i;
    }
  }
  return -1;
}

// Top-level comma-separated items between open and close (exclusive).
function items(masked, open, close) {
  const out = [];
  let depth = 0;
  let start = open + 1;
  for (let i = open + 1; i < close; i++) {
    const c = masked[i];
    if ("({[".includes(c)) depth++;
    else if (")}]".includes(c)) depth--;
    else if (c === "," && depth === 0) {
      out.push([start, i]);
      start = i + 1;
    }
  }
  if (masked.slice(start, close).trim()) out.push([start, close]);
  return out;
}

const registered = new Map();
const adders = new Set();
for (const [file, text] of Object.entries(sources)) {
  const masked = code(text);
  for (const fn of masked.matchAll(/\bvoid\s+(Add[A-Za-z]+Handlers)\s*\(\s*TArray<FHandlerEntry>\s*&\s*Out\s*\)\s*\{/g)) {
    adders.add(fn[1]);
    const bodyOpen = fn.index + fn[0].length - 1;
    const bodyClose = closing(masked, bodyOpen);
    for (const add of masked.slice(bodyOpen, bodyClose).matchAll(/\bOut\.(Add|Append)\s*\(/g)) {
      const callOpen = bodyOpen + add.index + add[0].length - 1;
      if (add[1] === "Append") {
        errors.push(`C2: ${file}: ${fn[1]} uses Out.Append; register each handler with Out.Add({ name, fn, params, rules })`);
        continue;
      }
      const brace = masked.indexOf("{", callOpen);
      const entry = items(masked, brace, closing(masked, brace));
      const name = /^\s*TEXT\("(voxel_[a-z0-9_]+)"\)\s*$/.exec(text.slice(...entry[0]))?.[1];
      if (!name) {
        errors.push(`C2: ${file}: an entry in ${fn[1]} does not start with TEXT("voxel_...")`);
        continue;
      }
      if (registered.has(name)) errors.push(`C2: ${name} is registered twice`);
      if (entry.length < 3) errors.push(`C2: ${name} is registered without a parameter contract`);
      if (entry.length > 4) errors.push(`C2: ${name}: an entry is { name, fn, params, rules? }, got ${entry.length} items`);
      registered.set(name, { file, params: entry[2] ? text.slice(...entry[2]) : "" });
    }
  }
}
const common = sources["VoxelToolsCommon.cpp"] ?? "";
for (const adder of adders) if (!new RegExp(`\\b${adder}\\s*\\(\\s*Out\\s*\\)`).test(common)) errors.push(`C2: GetHandlers never calls ${adder}`);

const handlers = native.handlers ?? {};
for (const name of registered.keys()) if (!handlers[name]) errors.push(`C2: ${name} is registered in C++ but missing from the manifest`);
for (const name of Object.keys(handlers)) if (!registered.has(name)) errors.push(`C2: ${name} is in the manifest but has no C++ registration with a contract`);
ok(`C2 ${registered.size} C++ registrations with contracts, ${Object.keys(handlers).length} manifest handlers`);

// C3: the module registers through the contract overload, and no declared name is one the dispatcher consumes.
const moduleSrc = sources["VoxelPluginToolsModule.cpp"] ?? "";
if (!/UEMCP::RegisterExternalHandler\(\s*Entry\.Name\s*,[^;]*Entry\.Params\s*,\s*Entry\.Rules\s*,/.test(moduleSrc)) {
  errors.push("C3: VoxelPluginToolsModule.cpp does not register Entry.Params and Entry.Rules through UEMCP::RegisterExternalHandler");
}
if (/RegisterExternalHandlerWithTimeout/.test(moduleSrc)) errors.push("C3: RegisterExternalHandlerWithTimeout drops the contract; pass the timeout to the contract overload");
for (const [file, text] of Object.entries(sources)) {
  for (const m of text.matchAll(/MCPParam::(?:Required|Optional)(?:Field)?\(\s*TEXT\("([^"]+)"\)/g)) {
    if (ROUTING.has(m[1])) errors.push(`C3: ${file} declares '${m[1]}', a routing name the bridge refuses`);
  }
}
ok("C3 contract registration and declared names checked");

// C4: every handler declares its effect and a description; the server appends the Params clause from the contract,
// and a manifest schema would describe parameters the contract already owns.
for (const [name, spec] of Object.entries(handlers)) {
  if (!["read", "mutate"].includes(spec.effect)) errors.push(`C4: ${name}: effect must be read or mutate`);
  if (!spec.description) errors.push(`C4: ${name}: no description`);
  else if (/\bParams:/.test(spec.description)) errors.push(`C4: ${name}: description has a hand-written Params: clause; the server generates it`);
  if (spec.schema) errors.push(`C4: ${name}: schema: is replaced by the C++ contract`);
}
ok("C4 effects and descriptions checked");

// C5: flows reference native actions that exist.
for (const [flow, spec] of Object.entries(manifest.flows ?? {})) {
  for (const [step, s] of Object.entries(spec.steps ?? {})) {
    const m = /^voxel\.(voxel_[a-z0-9_]+)$/.exec(s.task ?? "");
    if (m && !handlers[m[1]]) errors.push(`C5: flow ${flow} step ${step} calls unknown action ${s.task}`);
  }
}
ok("C5 flow steps resolve");

// C6: the module's read-only set (no save bookkeeping) matches effect: read. Whether each contract declares save
// exactly when the handler mutates is checked against the live contracts by scripts/live-test.mjs.
const readBlock = /ReadHandlers =\s*\{([\s\S]*?)\};/.exec(moduleSrc)?.[1] ?? "";
const moduleRead = new Set([...readBlock.matchAll(/TEXT\("(voxel_[a-z0-9_]+)"\)/g)].map((m) => m[1]));
const manifestRead = new Set(Object.entries(handlers).filter(([, s]) => s.effect === "read").map(([n]) => n));
for (const n of manifestRead) if (!moduleRead.has(n)) errors.push(`C6: ${n} is effect: read but missing from ReadHandlers`);
for (const n of moduleRead) if (!manifestRead.has(n)) errors.push(`C6: ${n} is in ReadHandlers but not effect: read`);
ok(`C6 ${moduleRead.size} read-only handlers agree`);

// C7: every action the docs name exists. `ue-mcp plugin check-skills` reads only SKILL.md and only the
// category(action="x") form; this covers the knowledge file, the README and every skill file, and bare names too.
const docFiles = ["README.md", ...readdirSync(resolve(root, "knowledge")).map((f) => `knowledge/${f}`)];
const walk = (dir) => readdirSync(resolve(root, dir), { withFileTypes: true }).flatMap((e) =>
  e.isDirectory() ? walk(`${dir}/${e.name}`) : e.name.endsWith(".md") ? [`${dir}/${e.name}`] : []);
if (existsSync(resolve(root, "skills"))) docFiles.push(...walk("skills"));
let named = 0;
for (const file of docFiles) {
  const text = readFileSync(resolve(root, file), "utf8");
  for (const m of text.matchAll(/\bvoxel\s*\(\s*action\s*[=:]\s*["']([A-Za-z0-9_]+)["']|\b(voxel_[a-z0-9_]+)\b/g)) {
    const name = m[1] ?? m[2];
    named++;
    if (!handlers[name]) errors.push(`C7: ${file} names ${name}, which is not a voxel action`);
  }
}
ok(`C7 ${named} action names in ${docFiles.length} doc files checked`);

if (errors.length) {
  for (const e of errors) console.error(`  ERR ${e}`);
  console.error(`errors: ${errors.length}`);
  process.exit(1);
}
console.log("errors: 0");
