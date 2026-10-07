---
name: voxel-graphs
description: "Use when authoring or debugging Voxel Plugin 2 graphs through ue-mcp: height, volume, scatter, spline, sculpt, smart surface and Voxel PCG graphs, and function libraries. Covers reading a graph, finding node types, adding and wiring nodes, pin defaults, graph parameters and their getters, functions and their inputs and outputs, graph instances, copying nodes as T3D, and checking what a graph outputs. Pulls in any time a voxel graph is created, edited or inspected."
---

# Voxel graph authoring with ue-mcp

A voxel graph is an asset holding one or more terminal graphs (the main graph, plus functions in a function library), each an editor node graph. The graph actions edit those nodes the way Voxel's graph editor does, recompile the graph and save it. Every edit is undoable.

The read-then-write loop:

1. `voxel(action="voxel_graph_read", assetPath="/Game/Terrain/HG_Terrain")`: terminal graphs (with GUIDs), parameters (name, guid, type, default) and every node with its id, title, pins, pin defaults and links. Pass `includePins: false` for a large graph's outline.
2. `voxel(action="voxel_graph_list_node_types", assetPath=..., query="noise 2d")`: the node types this graph type allows, as `Category|Name` keys. Query words must all match the key or the tooltip. The list includes exposed function-library functions and `Parameters|<name>` getters.
3. `voxel(action="voxel_graph_add_node", assetPath=..., nodeType="Math|Misc|Smooth Step", x=-400, y=200)`: returns the node with its `id` and pins. A bare name works when it is unique.
4. `voxel(action="voxel_graph_connect", assetPath=..., fromNode=<id>, fromPin="Result", toNode=<id>, toPin="Height")`. Nodes take an id (GUID), object name or unique title; pins take a name or display name. A connection the schema disallows is refused with its reason.
5. `voxel(action="voxel_graph_set_pin_default", assetPath=..., node=<id>, pin="B", value=5000)` for an unconnected input; a connected pin's default is unused and refused.
6. `voxel(action="voxel_graph_disconnect")` (same arguments as connect) breaks one link; `voxel(action="voxel_graph_delete_node", node=<id>)` removes a node and its links.
7. `voxel_graph_read` again to confirm the shape, then check the output (below).

Function libraries have no main graph: pass `terminalGraph` (a GUID from `voxel_graph_read`) to every action on one. `assetPath` takes the library asset itself.

A height graph ends in Output Height: `Height`, `SurfaceType` (a surface type blend), `Bounds` (the stamp's extent; Voxel's template feeds it from Make Box 2D From Radius, so widen it when the shape grows), `HeightRange`, and metadata pins added in pairs. A volume graph ends in Output Volume with `Distance` (negative inside), `SurfaceType` and `Bounds`. Advanced pins on both outputs override layer, blend mode and alpha per graph. Useful starting keys: `Noise|Advanced Noise 2D`, `Noise|Perlin Noise 2D`, `Math|Misc|Lerp`, `Math|Misc|Smooth Step`, `Math|Mask Generators|Box Falloff 2D` and `Sphere Falloff 2D` (functions from Voxel's stock function library), `Surface Type|Make Surface Type Blend`, `Surface Type|Blend Surface Types`. Confirm a key with `voxel_graph_list_node_types` before adding it.

## Parameters

Parameters are what stamps, instances and PCG override per use, so every value a designer should tune is a parameter, not a pin default.

1. `voxel(action="voxel_graph_add_parameter", assetPath=..., name="Amplitude", type="float", default=3000)`. Types: `float`, `double`, `int32`, `int64`, `bool`, `name`, `vector2d`, `vector`, `color`, `seed`, or `struct:`, `object:`, `class:`, `enum:` with a path (a surface type parameter is `struct:/Script/Voxel.VoxelSurfaceType`). The default is parsed against the type before the graph changes.
2. `voxel(action="voxel_graph_add_node", assetPath=..., nodeType="Parameters|Amplitude")` places its getter; wire the getter's output where the value is used. A parameter with no getter does nothing.
3. `voxel(action="voxel_graph_set_parameter_default")` changes the default; `voxel(action="voxel_graph_remove_parameter")` removes the parameter, its getters in every terminal graph and its stored value.

Stamps override parameters by name (`voxel(action="voxel_stamp_set")` `parameters`, `voxel(action="voxel_stamp_set_parameters")`); see the terrain skill.

## Instances

A variant that differs only in parameter values is an instance, not a copy: `voxel(action="voxel_asset_create", type="height_graph", name="HG_Hills", instanceOf="/Game/Terrain/HG_Terrain")`, then `voxel_graph_set_parameter_default` on the instance, which sets the instance's own value and leaves the base alone. An instance has no main graph of its own, so these actions refuse node edits and new parameters on it; edit the base and every instance follows. `voxel_graph_read` on an instance names its `baseGraph` and reads the nodes from it (`nodesFrom`).

## Functions

A function is a terminal graph of its own. A graph calls its own functions; a function library's exposed functions appear in every graph's node types.

1. `voxel(action="voxel_asset_create", type="function_library", name="FL_Masks", packagePath="/Game/Terrain")` creates a library holding one empty function named after the asset.
2. `voxel(action="voxel_graph_add_function", assetPath="/Game/Terrain/FL_Masks", name="Ridge", category="Masks", inputs=[{"name": "Height", "type": "float buffer"}], outputs=[{"name": "Mask", "type": "float buffer"}])` returns `function.guid` and `nodes`: one Function Input node per input and one Function Output node per output, each with a `Value` pin. Member types are the parameter types, optionally with Voxel's ` buffer` or ` array` suffix; values that vary per point are buffers. An input's `default` is parsed as its type. Inputs and outputs are declared here only: no action adds one to an existing function.
3. Build the body with the loop above, passing `terminalGraph=<function.guid>`. Inside a function, `voxel_graph_list_node_types` also offers `Function Inputs|Get <name>` and `Function Outputs|Set <name>` for further input and output nodes.
4. `voxel(action="voxel_graph_set_function", assetPath=..., terminalGraph=..., name="RidgeMask")` renames it; `category`, `description` and `exposeToLibrary` work the same way. In a library, `exposeToLibrary` (default true) decides whether other graphs list the function, as `Category|Name`, for `voxel_graph_add_node`.
5. `voxel(action="voxel_graph_remove_function", assetPath=..., terminalGraph=...)` is refused while any graph calls the function, and names the call nodes to delete with `voxel_graph_delete_node`. It loads every voxel graph in the project to find them, and closes the asset's editor if it is open.

`voxel_graph_read` lists every function under `terminalGraphs` with its category, description, inputs and outputs, plus `exposeToLibrary` in a library.

## Copying nodes

`voxel(action="voxel_graph_export_t3d", assetPath=..., nodes=[...])` returns clipboard text for the listed nodes (all copyable nodes when omitted); `voxel(action="voxel_graph_import_t3d", assetPath=<target>, t3d=..., offsetX=600)` pastes it with new GUIDs, keeping links inside the text. Voxel only accepts pastes into an open graph editor, so the import opens it for the paste and closes it again. Nodes the target graph type does not allow, and output nodes, are dropped and reported in `warning`.

## Checking what a graph outputs

A call that succeeds proves the graph compiled, not that it produces the intended shape. Sample it:

1. Stamp the graph (terrain skill) and `voxel(action="voxel_query_layer")` at points across the feature, with the stamp's `stack` and `layer`.
2. To see an intermediate value, wire it into the output's `Height` pin, sample, then wire the real result back.
3. Capture the viewport for the shape a human will judge.

Traps:

- **Smooth Step and Lerp clamp their Alpha, not their output.** With `Clamp` true (the default) both compute on `clamp(Alpha, 0, 1)`. Smooth Step with A and B in centimetres and Clamp true reads every Alpha above 1 as 1, so the result is constant: set `Clamp` false whenever Alpha is not already 0..1. Lerp with Clamp true cannot extrapolate.
- **Inherited nodes are read-only.** An action on a graph instance's inherited terminal graph is refused with the base graph's path; edit that.
- **Edits recompile before saving.** Each edit flushes Voxel's graph compilation before the save, so the saved asset matches the edit; `save: false` leaves the graph dirty for you to save.

## Parameter overrides written from C++

A project that writes `FVoxelParameterValueOverride` itself (a generator spawning stamps) must store the value in the parameter's exposed type. For a surface type parameter that is an object of `UVoxelSurfaceTypeInterface` (the base of surface and smart surface types), not the inner `FVoxelSurfaceType` struct: build the value with `FVoxelPinValue(Parameter.Type.GetExposedType())` and `ImportFromUnrelated(FVoxelPinValue::Make(Surface))`, or call `IVoxelParameterOverridesOwner::SetParameter`, which fixes the value up to the exposed type itself. An override in the inner type fails `ImportFromUnrelated` during `FixupParameterOverrides`, is moved to an orphan (logged as `Orphaned parameter`), and the stamp falls back to the graph default.

## Before reporting done

- [ ] `voxel_graph_read` shows every new node wired and no getter left unconnected.
- [ ] Every tunable value is a parameter with a sensible default.
- [ ] Smooth Step and Lerp nodes fed by world-unit values have `Clamp` false.
- [ ] The output was sampled with `voxel_query_layer` on the right layer and seen in a capture.
