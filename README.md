# ue-mcp-plugin-voxel-plugin-pro

[Voxel Plugin Pro 2](https://docs.voxelplugin.com/) actions for [ue-mcp](https://github.com/db-lyon/ue-mcp).

Injects three actions into the existing built-in categories:

| Category    | Action                          | What it does                                                        |
|-------------|---------------------------------|---------------------------------------------------------------------|
| `pcg`       | `vpp_scatter_on_terrain`        | Builds a PCG graph that scatters static meshes on a voxel terrain.  |
| `pcg`       | `vpp_spawn_stamps`              | Builds a PCG graph that places voxel stamp assets.                  |
| `landscape` | `vpp_voxel_to_heightmap`        | Bakes a region of a voxel terrain into a standard Landscape heightmap. |

Plus two ready-to-run flows: `vpp_scatter_setup` and `vpp_stamps_setup`.

## Install

In your project directory:

```bash
ue-mcp plugin install ue-mcp-plugin-voxel-plugin-pro
```

The CLI runs `npm install --save`, validates the plugin manifest, adds an entry under `plugins:` in your `ue-mcp.yml`, and prints a restart instruction. Injected actions appear on the next ue-mcp server start.

## Requirements

- ue-mcp `>= 1.0.10`.
- Voxel Plugin Pro 2 enabled in your `.uproject`. The installer warns at install time if it's missing.

## Usage

The actions appear natively inside their host categories - no new tool to learn:

```text
pcg(action="vpp_scatter_on_terrain",
    graphPath="/Game/PCG/Scatter",
    mesh="/Game/Foliage/Rock.Rock",
    density=0.25)

pcg(action="execute", graphPath="/Game/PCG/Scatter")
```

Or via the bundled flow:

```text
flow(action="run", flowName="vpp_scatter_setup", params={
  graphPath: "/Game/PCG/Scatter",
  mesh: "/Game/Foliage/Rock.Rock"
})
```

See `knowledge/pcg.md` and `knowledge/landscape.md` for the full parameter reference. The same content is attached to the host category's AI-facing docs at server start so MCP agents see it natively.

## Develop

```bash
git clone https://github.com/db-lyon/ue-mcp-plugin-voxel-plugin-pro.git
cd ue-mcp-plugin-voxel-plugin-pro
npm install
npm run build
```

The plugin compiles to `dist/`. To test against a local checkout of ue-mcp, `npm link` this package and `npm link ue-mcp-plugin-voxel-plugin-pro` from your project directory.

## Why a plugin?

ue-mcp's plugin model injects new actions into existing built-in categories rather than creating a separate tool. The agent already working in `pcg` discovers `vpp_scatter_on_terrain` exactly when it's relevant - it does not need to know there is a separate Voxel Plugin Pro surface to open. See the [ue-mcp plugin docs](https://db-lyon.github.io/ue-mcp/plugins/) for the full author contract.

## License

MIT - see `LICENSE`.
