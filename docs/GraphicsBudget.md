# Graphics memory budget

The graphics setting `graphics_memory_budget_mb` caps how much GPU memory the optional
effects may take, in MiB (G13, docs/GraphicsImplementation.md section 17.3).

- `0`, the default, means no budget.
- Any other value must be a whole number from 16 to 65536.

## What it counts

The budget counts what the graphics settings can turn down. These are the categories of
the graphics report's `memory` block:

- temporal history
- ambient occlusion
- surface buffers (the depth prepass's normals)
- bloom
- depth pyramid
- fog
- transmission
- skin
- reflection probes
- anti-aliasing

It doesn't count the scene's own colour and depth targets. Shadow maps aren't counted
either: `shadow_quality` sizes them in its own tiers. Nor are meshes, textures
(`texture_quality` decides those) or per-frame data.

## What happens over budget

After each frame, the engine checks what the counted effects took. If they're over the
budget, the next step in this list is applied, one step per frame, until they fit:

| Step | Setting | Turned down to | Frees |
| --- | --- | --- | --- |
| 1 | `anti_aliasing` | `fxaa` | TAA's history and motion vectors |
| 2 | `skin_quality` | `off` | the skin diffusion targets |
| 3 | `transmission_quality` | `off` | glass's copy of the scene |
| 4 | `fog_quality` | `analytic` | the froxel volume |
| 5 | `ambient_occlusion` | `low` | part of the AO targets |
| 6 | `ambient_occlusion` | `off` | the AO targets, and the prepass if nothing else needs it |
| 7 | `reflection_probes` | `false` | resident probe cubes |
| 8 | `occlusion_culling` | `off` | the depth pyramid |
| 9 | `bloom_quality` | `low` | most of bloom's chain |

TAA's history goes first because it's the largest by far: about 68 MiB at 1080p, against
about 8 MiB for AO. FXAA still smooths edges without it.

A step is skipped if its effect holds no memory in this scene. A scene without glass never
loses its glass setting, because turning it off would save nothing.

If everything that could be turned down already has been and the effects are still over
budget, the engine logs it once and the report's `over_budget` is true. The budget never
drops geometry, never edits source assets, and never changes shadows or textures.

## The player's settings stay theirs

The steps change what the renderer runs, never the player's choice:

- `game.graphics().current()` and the settings menus show what the player chose.
- Saving writes what the player chose.
- Each step shows up as a fallback in `Graphics::status()`, with the reason
  "memory budget: optional effects took N MiB of B".
- In the graphics report, the step shows as requested `taa`, effective `fxaa`, with the
  same reason.

The engine starts over whenever settings are applied or the window changes size: it
restores everything the player chose, then fits again. Once the effects fit, it measures
again about once a second. That way a scene that brings in glass or skin later is still
caught.

## Measured

Arena's benchmark scene at 1920x1080, with TAA and high AO, without a budget: optional
effects take 91.7 MiB.

| Budget | Turned down | Optional effects after |
| --- | --- | --- |
| 64 MiB | `anti_aliasing` to `fxaa` | 34.4 MiB |
| 20 MiB | then `ambient_occlusion` to `off` | 13.2 MiB |

Skin, glass and fog were left alone at both budgets: Arena has none of them.

## Code

- `Engine/GraphicsBudget.h`: what's counted, the steps, and `nextMemoryBudgetStep`.
  `graphics_test` checks the order and the skipping.
- `Graphics::afterFrame`: measuring and applying the steps.
