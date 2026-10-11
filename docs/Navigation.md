# Navigation

Navigation lets characters find their own way around a level. Shockwave works out where
the ground is walkable, and characters with a `NavAgent` walk there during Play. They
take the shortest route round walls and steer round each other. You'd use it for
enemies, guards, crowds and anything else that has to get from A to B on its own.

The walkable area is called the navigation mesh, or navmesh: the floor of your level as
flat polygons. Shockwave builds it with [Recast](https://github.com/recastnavigation/recastnavigation)
and finds paths on it with Detour. Steering a group of characters is DetourCrowd's job.

New to Shockwave? [Tutorial: your first game in Shockwave](Tutorial.md) walks through a
whole project first.

## Try the sample

The sample project has a navigation scene. A red guard walks round four waypoints, and
a blue follower keeps up with the guard.

1. Open the sample project in the editor, then open `Scenes/navigation.swscene` from the
   Content panel.
2. Select the `Environment` entity, which holds the scene's `NavMeshSettings`. The
   viewport tints the walkable area. To see it all the time, open View in the viewport
   and turn on Navigation mesh.
3. Press Play. The orange lines show the path each character is taking.

## Make characters walk somewhere

1. Use Create > Navigation settings. This adds an entity named `Navigation` with a
   `NavMeshSettings` component and bakes the navmesh, which means it works out the
   walkable area from your scene. The editor saves it next to the scene as a
   `.swnavmesh` file.
2. Use Create > Nav agent. This adds a character with a `NavAgent` component, standing
   on the navmesh.
3. In the Inspector, set the agent's Destination and turn on Has destination. Or pick
   another entity as its Target, and it follows that entity.
4. Press Play.

Only things that don't move are walked on. That means a `Collider` with no `RigidBody`
or a Static one, and meshes (`MeshRenderer` and `ModelRenderer`). Anything with a
Dynamic or Kinematic `RigidBody` is left out.

When you change the level, the Bake button on `NavMeshSettings` says "out of date".
Press it to bake again. Only the parts of the level you changed are rebuilt. If you
forget, Play bakes a fresh navmesh for itself, which makes Play a little slower to
start.

## Move characters from game code

Game code reaches navigation through `game.navigation()`:

```cpp
#include <Shockwave/Game.h>
using namespace shockwave;

void beginPlay(GameContext& game) override {
    EntityRef guard = game.find("Guard");
    game.navigation().setDestination(guard, {10, 0, 4});   // walk to a point
    game.navigation().setTarget(guard, game.find("Player"));   // or follow someone
}
```

| Call | What it does |
|---|---|
| `setDestination(agent, point)` | Walk to a point. |
| `setTarget(agent, entity)` | Follow an entity. The route is updated as it moves. |
| `stop(agent)` | Stand still. |
| `findPath(start, end, points)` | Fill `points` with the corners of a walking route. Returns false when either end is off the navmesh. |
| `nearestPoint(point)` | The closest walkable spot, for example under a mouse click. |
| `raycast(start, end)` | Where a straight walk from start to end would hit a wall, if anywhere. |
| `reachable(start, end)` | Whether you can walk from one point to the other at all. |

Each agent's `NavAgent` also tells you how it's doing. `arrived` turns on when it gets
there, `remaining_distance` is how far it still has to go, and `velocity` is how fast
it's moving. The sample's `Patrol` component in `Games/Sample/Source/SampleGame.cpp`
uses `arrived` to send its guard to the next waypoint.

Characters with a `CharacterController` are moved by physics. The agent sets the
controller's `desiredVelocity`, so the character still collides, slides on walls and
falls. Characters without one are moved directly along the navmesh.

## NavAgent settings

```text
entity "Guard"
  Transform
    position 0 0 5
  NavAgent
    speed 3.5
    target "Player"
end
```

The agent's `Transform` position is its feet.

| Setting | Text name | What it does |
|---|---|---|
| Speed | `speed` | Top speed, in units per second. |
| Acceleration | `acceleration` | How quickly it speeds up and slows down. |
| Radius | `radius` | How wide it is. Keep it at or below the navmesh's `agent_radius`. |
| Height | `height` | How tall it is. |
| Stopping distance | `stopping_distance` | How close it gets before it counts as arrived. |
| Target | `target` | An entity to follow. Wins over the destination. |
| Destination | `destination` | A point to walk to, used when `has_destination` is on. |
| Has destination | `has_destination` | Turns the destination on. |
| Avoidance | `avoidance` | How hard it looks for gaps between other agents: None, Low, Medium or High. Higher costs more CPU. |
| Separation | `separation` | How strongly it keeps its distance from nearby agents. 0 turns it off. |
| Face movement | `face_movement` | Turns the character to face where it walks. |
| Enabled | `enabled` | Turn it off to take the agent out of the crowd. |

## NavMeshSettings

One entity per scene can have `NavMeshSettings`. A scene with agents and no settings
uses the defaults below.

| Setting | Text name | What it does |
|---|---|---|
| Geometry | `geometry` | What's walked on: Static colliders, Render meshes, or Both (the default). With Both, an entity's collider is used when it has one, and its mesh otherwise. |
| Agent radius | `agent_radius` | The navmesh keeps this far from walls. |
| Agent height | `agent_height` | Spaces with a lower ceiling are left out. |
| Max climb | `max_climb` | Ledges up to this high count as steps. |
| Max slope | `max_slope` | Slopes up to this many degrees are walkable. |
| Cell size | `cell_size` | How finely the level is sampled across. Smaller is more exact and slower to bake. About half of `agent_radius` works well. |
| Cell height | `cell_height` | How finely the level is sampled in height. |
| Tile size | `tile_size` | The navmesh is cut into square tiles this many cells wide. Tiles bake in parallel, and only changed tiles are rebaked. |
| Auto bounds | `auto_bounds` | Bake the whole level. Turn it off to bake only between `bounds_min` and `bounds_max`. |
| Max agents | `max_agents` | The most agents that can walk at once. Any more stand still. |

To change how one object is baked, give it a `NavModifier`:

| Mode | What it does |
|---|---|
| Walkable | Baked even if Geometry would skip it. |
| Not walkable | Blocks agents but is never walked on, like a table top. |
| Ignore | Left out completely, like a decal or a pickup. |

## Check and bake from the command line

Bake every scene's navmesh, save it, and print what it costs:

```sh
build/bin/player --bake-navmesh
```

You'll see a report like this for each scene that uses navigation. It also plays the
scene for 10 seconds without a window and says where each agent ended up:

```text
Scenes/navigation.swscene: 16 tiles, 67 polygons, 148 vertices from 300 triangles (8 objects) in 2.59 ms on 4 threads, 13.1 KB
  wrote Scenes/navigation.swnavmesh
  crowd: 2 agents, 0.0074 ms per tick on average, 0.0462 ms at worst, over 600 ticks
  agent 'Guard': at (-4.03, 0.10, -2.78), walking, 12.80 from its goal
```

Pass scene files to bake only those: `build/bin/player --bake-navmesh Scenes/level.swscene`.

`player --check` reports navigation problems: a saved navmesh that no longer matches its
scene, settings that can't work, agents standing off the navmesh, and goals they can't
reach.

```sh
build/bin/player --check
```

To see the navmesh and the paths in a screenshot:

```sh
build/bin/player --scene Games/Sample/Scenes/navigation.swscene --view navmesh --frames 120 --screenshot nav.png
```

## Performance

- Baking runs on the job system, one tile per job, so it uses every core. The sample
  scene bakes in a few milliseconds. Save the baked navmesh (the editor's Bake button,
  or `--bake-navmesh`) so Play and the shipped game load it instead of baking.
- Moving agents happens once per fixed tick on the main thread, before physics. Window >
  Stats shows its cost under Navigation. A crowd of a few dozen agents costs well under a
  tenth of a millisecond.
- Path queries don't allocate memory once they've warmed up.

## Threading

- `findPath`, `nearestPoint`, `raycast` and `reachable` are safe from any thread at any
  time during Play, including from systems.
- `setDestination`, `setTarget` and `stop` write the agent's `NavAgent`. Call them on the
  main thread, for example in `fixedUpdate` or `frame`, or from a system that writes
  `NavAgent`.

## How it works

When Play starts, the scene's navigation looks for a navmesh in this order: one baked
earlier in this run with the same inputs, the scene's saved `.swnavmesh`, and then a
fresh bake. A fingerprint of every input (the settings and each static object's place,
shape and mesh) tells whether a navmesh still matches its scene.

Each fixed tick, agents are added to or removed from the crowd to match the World. Their
routes are renewed when their goal moves, then DetourCrowd steers them all one step. The
`.swnavmesh` file stores Detour's tile data as is, so it's tied to the CPU's byte order.
That's the same on every PC and Mac Shockwave runs on.

## Limits

- The navmesh doesn't change during Play. Moving obstacles that agents walk round, and
  rebaking parts of the level while playing, aren't supported yet.
- There's one navmesh per scene, baked for one agent size. Agents much wider than
  `agent_radius` can brush through walls; `player --check` warns about them.
- Off-mesh links, such as jumps, ladders and doors, aren't supported yet.
