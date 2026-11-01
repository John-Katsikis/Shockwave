# Drawing: batching, occlusion culling and the pipeline cache

Graphics batch G12 (docs/GraphicsImplementation.md, section 16) makes crowded scenes cheaper
to draw without changing a pixel. It has three parts, and each can be switched off:

| Part | Setting or flag | Off means |
| --- | --- | --- |
| Instancing | `instancing` (graphics setting, default `true`) | one draw call per object |
| Occlusion culling and indirect draws | `occlusion_culling` (`on`/`off`, default `on`) | no culling pass, no depth pyramid |
| Pipeline cache | `EngineConfig::pipelineCache`, `player --no-pipeline-cache` | every start compiles every pipeline |

The graphics report's `drawing` block (`player --graphics-report --json`) shows what the
last frame did. The graphics window's *Effective* rows show the same for each setting,
with the reason when a setting didn't apply.

## Instancing

Every frame's draws go into one draw table, which the shaders index by
`gl_InstanceIndex`. When two draws are neighbours in that table and use the same
pipeline, index buffer and index range (the same mesh at the same level of detail), they
go out as one `vkCmdDrawIndexed` with two instances. Each instance still finds its own
transform, material and vertices through its own table entry, so nothing about how it
looks changes.

With instancing on, opaque draws are sorted by pipeline, then mesh, then distance, so that
copies of a mesh end up next to each other. Every pass uses this: shadows, the depth
prepass, the scene, the skin pass and TAA's reactive mask.

## Occlusion culling

When a frame already draws the **surface prepass** (because ambient occlusion, TAA or decals
are on), its depth shows what's in front. Occlusion culling reuses that depth:

1. **Depth pyramid.** `shaders/hzb.comp` reduces the depth into a pyramid. Each texel keeps
   the *farthest* depth of the pixels it covers (reverse-Z, so it takes the minimum).
   - Level 0 is half the screen, rounded up.
   - It's padded to a power of two on each side, so that every level halves exactly.
   - The padding holds the nearest depth, which never makes anything look hidden.
2. **Cull.** `shaders/cull.comp` runs one thread per opaque object in view:
   - It projects the corners of the object's bounding cube onto the screen.
   - It picks the level where that rectangle covers at most 4 texels a side.
   - It writes the object's indirect draw command: with no instance when every texel under
     the rectangle is nearer than the object's nearest corner, and with one instance
     otherwise.
3. **Draw.** The scene pass draws those objects with `vkCmdDrawIndexedIndirect`.
   - Runs that share a pipeline and mesh buffer go out as one multi-draw call where the GPU
     supports it.
   - Where it doesn't, each object gets its own indirect call.

The test is conservative:

- A single sky pixel under an object keeps it.
- An object crossing the near plane, or behind the camera, is always drawn.
- An object only partly on screen is judged on its on-screen part.

`Renderer/Culling.h` holds the same maths on the CPU. The `culling` test checks it against
a brute-force reference.

Occlusion culling does not run, and the report says why, when:

- there's no prepass this frame (it never adds one just to cull);
- the GPU can't start indirect draws at an instance (`drawIndirectFirstInstance`);
- the frame is a reflection-probe capture;
- nothing opaque and rigid is in view.

Shadows, glass and other see-through surfaces, and skinned meshes (whose bounds are their
bind pose) are never culled this way.

The report's `tested` and `occluded` counts are read from an earlier frame's GPU counters,
without waiting for them.

## Pipeline cache

Compiling pipelines is most of a cold start's GPU work. The Vulkan renderer keeps a
`VkPipelineCache` in the user's config folder, in `pipeline-cache/` (on Linux,
`~/.local/share/Shockwave/<game>/pipeline-cache/`). The file is named after the GPU's vendor
and device.

- **Loading.** The cache is loaded at start. It's replaced when any of these change: the
  driver, the GPU, the cache UUID the driver reports, or the shaders.
- **Saving.** It's written at shutdown, through a temporary file and a rename, so a crash
  never leaves half a file.
- **Report.** The report's `pipeline_cache` shows the file, how many bytes were loaded, and
  its state (`new`, `loaded`, `replaced: ...` or `off`).
- **Turning it off or clearing it.**
  - `player --no-pipeline-cache` runs without the cache.
  - `player --clear-pipeline-cache` deletes it first.
  - In code, set `EngineConfig::pipelineCache = false`.

Some drivers keep nothing in a pipeline cache: llvmpipe saves only the 32-byte header. Those
start no faster with it, and no slower either.

## Measured

On llvmpipe at 1280x720, with ambient occlusion on medium, the crowded city fixture
(`Tests/fixtures/graphics/Scenes/crowded-city.swscene`, 1635 entities) gave:

| | everything off | instancing and culling on |
| --- | --- | --- |
| objects drawn, all passes | 6949 | 6949 |
| draw calls | 6949 | 154 |
| opaque draws tested / hidden | n/a | 1592 / 1497 |
| frame (10 s benchmark) | 308-321 ms | 271-294 ms |
| scene pass | 86 ms | 73 ms |
| occlusion culling pass | n/a | 6 ms |
| depth pyramid memory | 0 | 2.7 MB |

A CPU renderer spends its time filling pixels, not on draw calls. On a real GPU, the
saving on draw calls and on the vertex work for hidden objects is larger. On a frame with
few objects, the culling pass's fixed cost (about 6 ms on llvmpipe, well under a
millisecond on a GPU) can exceed what it saves. Turn `occlusion_culling` off for such a
game.
