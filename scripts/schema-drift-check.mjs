// C3: every YAML `schema` key corresponds to an option the task actually reads.
// Walks each task's `extends` chain through src/ and checks the key appears as a
// read (`.key`, destructured `{ key }`, or `"key"` for set_editor_property-style).
// Run: node scripts/schema-drift-check.mjs
import { readFileSync, existsSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import yaml from "js-yaml";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const manifest = yaml.load(readFileSync(resolve(root, "ue-mcp.plugin.yml"), "utf8"));
const tasks = manifest.tasks ?? {};

// task key -> src file path (from class_path)
function srcFor(classPath) {
  const p = resolve(root, "src", `${classPath}.ts`);
  return existsSync(p) ? p : null;
}

// Given a src file, return its text plus the text of every ancestor in the
// `extends` chain (resolved via the matching local import).
function chainText(srcPath, seen = new Set()) {
  if (!srcPath || seen.has(srcPath)) return "";
  seen.add(srcPath);
  const text = readFileSync(srcPath, "utf8");
  // Match the CLASS declaration's extends (skip generic constraints like
  // `<TOpts extends VolumeSculptOptions>` and `interface X extends Y`).
  const ext = text.match(/class\s+[A-Za-z0-9_]+\s*(?:<[^>]*>)?\s+extends\s+([A-Za-z0-9_]+)/);
  if (!ext) return text;
  const base = ext[1];
  // find import: import { Base, ... } from "./X.js"  OR  import Base from "./X.js"
  const impRe = new RegExp(`import\\s+(?:\\{[^}]*\\b${base}\\b[^}]*\\}|${base})\\s+from\\s+["']([^"']+)["']`);
  const imp = text.match(impRe);
  if (!imp) return text; // base is external (e.g. ue-mcp/task) -> stop
  let rel = imp[1];
  if (!rel.startsWith(".")) return text; // external package
  rel = rel.replace(/\.js$/, ".ts");
  const basePath = resolve(dirname(srcPath), rel);
  return text + "\n" + chainText(basePath, seen);
}

// Collect option-interface field names from a chain's source text. Looks inside
// every `interface *Options { ... }` / `interface Options { ... }` block and the
// fields of those blocks (top-level `name?:` / `name:` declarations).
function optionFields(text) {
  const fields = new Set();
  const re = /interface\s+[A-Za-z0-9_]*Options\b[^{]*\{/g;
  let m;
  while ((m = re.exec(text))) {
    // walk braces from the opening { to find the block body
    let depth = 0, i = m.index + m[0].length - 1, start = i;
    for (; i < text.length; i++) {
      if (text[i] === "{") depth++;
      else if (text[i] === "}") { depth--; if (depth === 0) break; }
    }
    const body = text.slice(start + 1, i);
    // top-level fields only (depth 1 inside the interface)
    let d = 0;
    for (const line of body.split("\n")) {
      const fm = d === 0 && line.match(/^\s*([a-zA-Z0-9_]+)\s*\??\s*:/);
      if (fm) fields.add(fm[1]);
      for (const ch of line) { if (ch === "{") d++; else if (ch === "}") d--; }
    }
  }
  return fields;
}

const errors = [];
const warnings = [];
let checkedActions = 0;
let checkedKeys = 0;

for (const [cat, actions] of Object.entries(manifest.inject ?? {})) {
  for (const [action, def] of Object.entries(actions)) {
    const schema = def.schema ?? {};
    const schemaKeys = Object.keys(schema);
    const taskKey = def.task;
    const cp = tasks[taskKey]?.class_path;
    const src = srcFor(cp);
    if (!src) { errors.push(`${cat}.${action}: no src for ${taskKey} (${cp})`); continue; }
    const text = chainText(src);
    checkedActions++;
    for (const key of schemaKeys) {
      checkedKeys++;
      // read patterns: `.key`  | `{ ... key ... }` destructure | `"key"`/`'key'`
      const re = new RegExp(`(\\.\\s*${key}\\b)|(\\b${key}\\s*[,}:])|(["']${key}["'])`);
      if (!re.test(text)) {
        errors.push(`C3: ${cat}.${action} schema key '${key}' is never read in ${cp} (or its base chain)`);
      }
    }
    // reverse: option fields the task declares but the schema omits
    const schemaSet = new Set(schemaKeys);
    for (const f of optionFields(text)) {
      if (!schemaSet.has(f)) {
        warnings.push(`C3-rev: ${cat}.${action} option '${f}' (in ${cp} Options chain) is not in the manifest schema`);
      }
    }
  }
}

console.log("[C3] schema keys are read by the task (or a base class)");
console.log(`checked ${checkedKeys} schema keys across ${checkedActions} actions`);
console.log(`warnings: ${warnings.length}, errors: ${errors.length}`);
for (const w of warnings) console.log(`  WARN  ${w}`);
for (const e of errors) console.log(`  ERR   ${e}`);
process.exit(errors.length ? 1 : 0);
