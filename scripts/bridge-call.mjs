// Call one bridge method on the test project's editor: node scripts/bridge-call.mjs <method> [paramsJson|@file]
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const project = path.join(root, "tests", "voxel_plugin_tools");
const { EditorBridge } = await import(
  "file://" + path.join(project, "node_modules", "ue-mcp", "dist", "bridge", "bridge.js").replace(/\\/g, "/")
);

const [method, raw = "{}"] = process.argv.slice(2);
if (!method) {
  console.error("usage: node scripts/bridge-call.mjs <method> [paramsJson|@file]");
  process.exit(2);
}
const params = JSON.parse(raw.startsWith("@") ? fs.readFileSync(raw.slice(1), "utf8") : raw);

const bridge = new EditorBridge("127.0.0.1", 0);
bridge.setProjectContext(path.join(project, "voxel_plugin_tools.uproject"));
await bridge.connect(10000);
try {
  const reply = await bridge.call(method, params, 120000);
  console.log(JSON.stringify(reply, null, 2));
} finally {
  bridge.disconnect?.();
  process.exit(0);
}
