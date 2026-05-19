# ue-mcp-plugin-voxel-plugin

[Voxel Plugin](https://voxelplugin.com) actions for [ue-mcp](https://github.com/db-lyon/ue-mcp).

Injects three actions into the existing built-in categories:

| Category    | Action                    | What it does                                                          |
|-------------|---------------------------|-----------------------------------------------------------------------|
| `pcg`       | `voxel_scatter_meshes`    | Builds a PCG graph that scatters static meshes on a voxel terrain.    |
| `pcg`       | `voxel_spawn_stamps`      | Builds a PCG graph that places voxel stamp assets.                    |
| `landscape` | `voxel_bake_heightmap`    | Bakes a region of a voxel terrain into a standard Landscape heightmap.|

Plus two ready-to-run flows: `voxel_scatter_setup` and `voxel_stamps_setup`.

## Install

In your project directory:

```bash
ue-mcp plugin install ue-mcp-plugin-voxel-plugin
```

The CLI runs `npm install --save`, validates the plugin manifest, adds an entry under `plugins:` in your `ue-mcp.yml`, and prints a restart instruction. Injected actions appear on the next ue-mcp server start.

## Requirements

- ue-mcp `>= 1.0.15`.
- Voxel Plugin enabled in your `.uproject` (`Plugins[].Name == "Voxel"`). The installer warns at install time if it's missing.
- An active Voxel Plugin Pro subscription if your project's use case requires it - the upstream source repo is public-readable but commercial use is gated by the upstream license.

## Usage

The actions appear natively inside their host categories - no new tool to learn:

```text
pcg(action="voxel_scatter_meshes",
    graphPath="/Game/PCG/Scatter",
    mesh="/Game/Foliage/Rock.Rock",
    density=0.25)

pcg(action="execute", graphPath="/Game/PCG/Scatter")
```

Or via the bundled flow:

```text
flow(action="run", flowName="voxel_scatter_setup", params={
  graphPath: "/Game/PCG/Scatter",
  mesh: "/Game/Foliage/Rock.Rock"
})
```

See `knowledge/pcg.md` and `knowledge/landscape.md` for the full parameter reference. The same content is attached to the host category's AI-facing docs at server start so MCP agents see it natively.

## Develop

```bash
git clone https://github.com/db-lyon/ue-mcp-plugin-voxel-plugin.git
cd ue-mcp-plugin-voxel-plugin
npm install
npm run build
```

The plugin compiles to `dist/`. To test against a local checkout of ue-mcp, `npm link` this package and `npm link ue-mcp-plugin-voxel-plugin` from your project directory.

## Why a plugin?

ue-mcp's plugin model injects new actions into existing built-in categories rather than creating a separate tool. The agent already working in `pcg` discovers `voxel_scatter_meshes` exactly when it's relevant - it does not need to know there is a separate Voxel surface to open. See the [ue-mcp plugin docs](https://db-lyon.github.io/ue-mcp/plugins/) for the full author contract.

## License

MIT - see `LICENSE`.
