# ue-mcp-plugin-voxel-plugin

[Voxel Plugin](https://voxelplugin.com) actions for [ue-mcp](https://github.com/db-lyon/ue-mcp).

## Status

**v0.2.0 ships no actions.** The previous `0.1.x` releases injected three
placeholder actions (`voxel_scatter_meshes`, `voxel_spawn_stamps`,
`voxel_bake_heightmap`) that called ue-mcp tasks with wrong parameter names
and passed PCG node-type strings that do not exist. They have been removed
rather than left as broken surface.

Real tools, grounded in the actual Voxel Plugin C++ API, are tracked in
[`TODO.md`](TODO.md). Every entry there cites the header file and class it
wraps.

Do not depend on this package yet — it intentionally exposes nothing.

## Requirements (for future versions)

- ue-mcp `>= 1.0.15`
- Voxel Plugin enabled in your `.uproject` (`Plugins[].Name == "Voxel"`)

## Develop

```bash
git clone https://github.com/db-lyon/ue-mcp-plugin-voxel-plugin.git
cd ue-mcp-plugin-voxel-plugin
npm install
npm run build
```

## License

MIT - see `LICENSE`.
