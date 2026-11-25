# Material graphs

A material graph (`.swmatgraph`, W4) builds a material's surface by connecting nodes: a
texture scrolling with time, noise masking a glow, a colour a material can change. It
compiles to one GLSL function that fills in the standard surface (base colour, opacity,
normal, roughness, metallic, emissive, occlusion). The renderer splices it into the
scene's fragment shader at the `@material_graph` call in `mesh.glsl`; lighting, shadows,
fog, reflections and everything after are the engine's own, unchanged.

A `.swmaterial` draws with a graph through its `graph` record, and changes the graph's
params with `graph_param` (docs/MaterialSchema.md). The node reference is
docs/MaterialGraphNodes.md (`player --material-graph-schema`).

## The file

```
swmatgraph 1
node rock texture "textures/rock.png"           # id, type, then the type's own values
node mask texture "textures/mask.png" linear    # "linear": data, not colour
node flow panner speed=speed                     # inputs: pin=node[.output] or pin=number
node speed constant 0.1 0
node glow param glow 6 2 0.3                     # a named value a material can change
node hot multiply a=glow b=mask.r
output base_color rock.rgb
output emissive hot

# layout rock 40 120                             # editor positions, kept at the end
```

- One record per line; `#` starts a comment (except a `# layout` line, which places a
  node in the editor). The first record is `swmatgraph 1`.
- `node <id> <type> ...`: the id is letters, digits and `_`. What follows is the type's
  values (a constant's numbers, a texture's quoted path, a param's name and default) and
  its inputs, `pin=node`, `pin=node.output` or `pin=number`.
- `output <surface> <node>[.output]`: base_color, opacity, normal, roughness, metallic,
  emissive or occlusion. An output the graph doesn't write keeps the material's own value
  (its colour, texture, roughness and so on, as a standard material works them out).
- What travels on a wire is a float or a vec2, vec3 or vec4. A float connects to any
  vector by repeating; most maths nodes take the widest of what's connected to them, and
  two different vectors don't mix.
- At most 256 nodes, 16 textures and 16 params.

Saving from the editor writes the canonical order (nodes, outputs, layout) and drops other
comments.

## What it does and doesn't change

- `opacity` matters with `alpha_mode blend`; `alpha_mode mask` still cuts out with the base
  colour texture's alpha (shadows and the depth prepass don't run the graph).
- `normal` is in tangent space, blue up. The shader builds a tangent frame for it, as it
  does for a normal map.
- `emissive` replaces the material's emissive; `occlusion` darkens ambient light only.
- `time` is the game's frame clock, which repeats exactly under `--fixed-step`.
- Graphs belong to family standard. A graph on glass, skin, hair or water does nothing, and
  the material says so.
- The prepass, screen-space effects and shadows see the material's own normal and alpha,
  not the graph's.

## Compiling and the shader cache

The editor, `player --check` and asset loading compile a graph with glslangValidator, the
program the build compiled the engine's shaders with (cmake/Shaders.cmake; set
`SHOCKWAVE_GLSLANG` to use another, or `none` for none). The result goes in the project's
`Saved/Cache/Shaders/<hash>.spv`, named by a hash of the generated shader and of the
engine's own compiled scene shader, so a graph compiled against another engine build is
compiled again. Compiling a graph takes a fraction of a second; after that it's read from
the cache.

A packaged game never compiles: it reads the `.spv` files its package ships. Each graph
requires its compiled shader in the asset graph, so a graph nobody has compiled since it
changed fails `player --check` and the cook (`AssetMissing`); `--check` compiles every
graph in Content first, so running it is enough.

There's no fixed limit on how many graphs a game has. Each level builds only the graphs
its materials use, while it loads: each graph's shader has its own pipelines, two built at
once and the rest the first time a draw needs them (double-sided, mirrored or blended
draws). `--check` prints how many graphs each scene loads and warns past 64, where loading
starts to take noticeably longer. The graphics report's `material_graphs` section counts
the graph shaders alive, last frame's graph draws, and how many graphs were compiled or
read from the cache and how long that took.

## Switching it off

The graphics setting `material_graphs` (default true) turns graphs off: every graph
material draws as its plain material, exactly as without its `graph` record, and no graph
pipelines are built. It's in the Graphics window and every tier.

## Tools

```sh
player --material-graph Content/materials/lava.swmatgraph [--json out.json]
player --material-graph Content/materials/lava.swmatgraph --dump
player --material-graph-schema docs/MaterialGraphNodes.md
player --check
```

`--material-graph` checks and compiles one graph (into its project's cache) and reports
its nodes, outputs, params and textures, or each mistake against its node, exiting 1 on
any. `--dump` prints the fragment shader it makes. Compiler errors in the generated GLSL
are mapped back to the node whose line they're on.

## Testing

- `material_graph` (unit): the parser and writer, every check `compileMaterialGraph`
  makes, the GLSL it generates, the cache names, `.swmaterial`'s `graph` and `graph_param`,
  and an AssetFolder compiling and loading a graph material on the null renderer (a broken
  graph leaves the plain material and names its node; editing the graph swaps the shader).
- `graphics_material_graph_render` (GPU, Tools/material_graph.py): a graph that works out a
  standard material's values in nodes draws the same picture (RMSE 0.001), `material_graphs
  false` draws exactly the plain material, the lava graph glows and `graph_param glow 0 0 0`
  takes it away, two fixed-step runs are identical, and the report counts the draws.
- `player_cli`: `--material-graph` on a broken graph, `--dump`, the schema.
