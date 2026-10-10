# Physics

Shockwave uses the Jolt physics engine (version 5.6.0) for collisions, falling objects,
triggers and a walking character. You set things up with components in the editor, and
your game code can react to contacts, cast rays and push objects. Physics runs the same
way in the editor's Play mode and in the standalone player.

New to Shockwave? [Tutorial: your first game in Shockwave](Tutorial.md) is the place to
start.

## Try the demo

```sh
./build/bin/editor Games/Sample/Scenes/physics.swscene --play
```

This opens the physics demo in the editor and presses Play. It has falling boxes and
spheres, a moving platform, a trigger, collision built from imported models, and a
character. To run it in the player instead:

```sh
./build/bin/player --scene Games/Sample/Scenes/physics.swscene
```

## Make an object solid or falling

Add components with the Inspector's Component menu, then save the scene as usual.

| You want | Add these components |
|---|---|
| A wall or floor that never moves | `Transform` and `Collider` |
| An object that falls and bounces | `Transform`, `Collider` and `RigidBody` set to Dynamic |
| An object your code moves, like a lift | `Transform`, `Collider` and `RigidBody` set to Kinematic |
| A player that walks and jumps | `Transform` and `CharacterController` (see below) |

A `RigidBody` can also be Static. A static body stays put unless your code moves it.

## Collider shapes

| Shape | Size setting |
|---|---|
| Box | Half extents: half the width, height and depth |
| Sphere | Radius |
| Capsule | Half height of the middle cylinder, plus a round cap of the given radius at each end |
| ConvexModel | Uses the entity's model, wrapped in one convex hull |
| TriangleModel | Uses the entity's model's exact triangles |

Every shape has an Offset, in the entity's local coordinates.

The two model shapes need a `ModelRenderer` on the same entity. They include every mesh
and node of the imported model. A convex hull is like shrink-wrapping the model, so any
holes or hollows get filled in. Triangle models can only be static. When the model file
reloads, its collision shape is rebuilt.

## Rigid body settings

A `RigidBody` sets:

- mass, friction, restitution (bounciness) and damping (how fast motion slows down)
- a gravity multiplier
- starting velocity, both linear and angular. Angular velocity is in radians per second.
- a collision category and mask (see [Choose what collides](#choose-what-collides))
- whether it is a sensor
- continuous collision detection, which stops fast objects from passing through thin
  walls. It applies to dynamic bodies that aren't sensors.

The full list of fields, ranges and defaults is in [SceneSchema](SceneSchema.md).

## Make a trigger zone

Turn on Sensor in a `RigidBody` to make a trigger. A sensor reports when things enter
and leave it, but doesn't push anything.

- A static sensor detects moving bodies.
- A kinematic sensor also detects static geometry.
- Sensors don't detect other sensors.
- A body that falls asleep (stops being simulated because it came to rest) stays
  counted as inside the sensor.

See [React to contacts and triggers](#react-to-contacts-and-triggers) for the code side.

## Change gravity and accuracy

Put one `PhysicsSettings` component on any entity in the scene. It sets:

- gravity
- collision steps, from 1 to 8. These are substeps per fixed tick. More steps are more
  accurate and cost more.

Without it, gravity is `(0, -9.81, 0)` and there is one step per fixed tick.

## Add a walking character

Add a `CharacterController` and a `Transform` to an entity. Don't add a `Collider` or a
`RigidBody`. The controller gives you:

- an upright capsule, with Y as up and the feet at `Transform.position`
- gravity, walking on slopes and staying on the ground
- stepping up stairs and jumping
- pushing dynamic bodies

Put the visible mesh on a separate child entity.

Your game code drives it each tick:

| Field | Who writes it | Meaning |
|---|---|---|
| `desiredVelocity` | Your code | Horizontal speed and direction in world units per second |
| `jumpRequested` | Your code | Set to true for one tick to jump |
| `velocity` | Physics | The actual velocity |
| `grounded` | Physics | True when standing on something |

These four fields are runtime only and are not saved.

```cpp
if (EntityRef player = game.find("Player")) {
    auto& character = player.get<CharacterController>();
    character.desiredVelocity = {2.0f, 0.0f, 0.0f};
    if (game.input().wasActionPressed("jump")) character.jumpRequested = true;
}
```

The controller has an inner body that queries can hit. It is not a complete player
with animation. It doesn't connect the keyboard, a gamepad or the camera for you.

## React to contacts and triggers

`<Shockwave/Game.h>` includes the physics API. During Play, `game.physics()` returns the
session's `PhysicsWorld`. It's available from `configureSystems` through `endPlay`. See
[Projects](Projects.md) for how game code is set up.

```cpp
#include <Shockwave/Game.h>
using namespace shockwave;

class MyGameMode final : public GameMode {
public:
    void fixedUpdate(GameContext& game, double) override {
        for (const auto& event : game.physics().contacts()) {
            if (event.sensor && event.type == ContactEvent::Type::Begin) {
                // event.a is the sensor; event.b is the overlapping entity.
                // Activate a checkpoint, door, or other gameplay behavior here.
            }
        }
        // Direction is normalized by raycast; hit distance is in world units.
        if (auto hit = game.physics().raycast({0, 5, 0}, {0, -1, 0}, 10)) {
            // hit->entity, point, normal and distance identify the closest hit.
        }
    }
};
```

`contacts()` lists this tick's events. Each is Begin, Stay or End. It's filled after
the physics step, on the main thread.

- Normal rigid-body contacts follow Jolt's contact rules. When bodies fall asleep, their
  contacts end.
- Sensor events track overlap even while bodies sleep. Use them for "entered" and
  "left" logic.
- An End event may name an entity that was destroyed. Check `world.alive()` before you
  touch its components.
- Contact normals point from `a` toward `b`.

## Push objects and cast rays

| Call | What it does |
|---|---|
| `addImpulse(entity, impulse)` | Changes the body's momentum right away, like a hit |
| `addForce(entity, force)` | Applies a force on the next step, like a thruster |
| `setVelocity(entity, linear, angular)` | Sets the velocity directly |
| `raycast(origin, direction, maxDistance, ...)` | Returns the closest hit, if any |

The first three work only on dynamic bodies. They return false for an invalid or
destroyed entity, and for static, kinematic or character entities. To move a kinematic
body, change its `Transform` instead.

A raycast can also ignore one entity, include sensors (off by default) and use a mask to
pick which categories it hits.

The session adds new bodies after `beginPlay` returns. If you need a query or impulse
inside `beginPlay`, call `game.physics().synchronize()` first.

## Choose what collides

Each body picks a category from 0 to 15, and a 16-bit mask of the categories it collides
with. Two bodies collide only when each one's mask includes the other's category. A
raycast's mask selects which categories it can hit.

## Scale, parents and editing during Play

- Boxes and imported meshes can be stretched unevenly.
- Spheres, capsules and characters need the same scale on all axes.
- Dynamic bodies and characters need parents that are scaled the same on all axes.
- Sheared transforms aren't supported. A character's scale must be positive.
- When physics moves a dynamic body, the result is written back to its local transform,
  and your authored scale is kept.

While playing:

- Editing a dynamic body's `Transform` teleports it.
- Changing physics settings or collision shape rebuilds the body.
- Changing a kinematic body's `Transform` sets a target. On the next tick the body
  moves there with a velocity, rather than teleporting.

## Settings and errors

Physics components save in `.swscene` files and appear in the Inspector, styled by the
theme like every other component.

If a body is set up wrong, it is skipped. The error shows on that entity in the
Inspector and in the Renderer panel. The player refuses to start with physics errors in
the scene.

Turning off a `Collider`, `RigidBody` or `CharacterController` removes that entity from
physics until you turn it back on.

## Threads

The physics step and synchronization happen on the main thread, between gameplay system
runs. Scheduled systems can move kinematic transforms or set character input before the
step.

Queries, impulses, forces and velocity calls use Jolt's locking. Gameplay worker threads
can call them between steps. Don't change the World at the same time as synchronization
or a step. No game code or ECS change runs inside Jolt's own contact callbacks.

## Run the tests

```sh
ctest --test-dir build --output-on-failure
```

This runs the CPU physics tests. They cover contacts, sensor overlap while sleeping,
filtering, forces, kinematic targets, raycasts, characters and stairs, imported geometry
and reload, parenting, saving scenes, and pausing, restarting and restoring a session.
The editor and player can also do short smoke runs with `--frames N`.

## Not supported yet

Joints, vehicles, ragdolls, character animation, a collision debug view and saved,
precomputed collision data.

## How it works

Shockwave builds Jolt through CMake. Each Play session, in the editor or the player, has
one `PhysicsWorld`. It's a subsystem (see `Engine/Subsystem.h`) that the `PlaySession`
adds when Play starts and destroys when Play stops. It runs on Shockwave's existing
worker threads.

The fixed tick runs at 60 Hz by default. Each tick runs your scheduled systems, then
physics, then `GameMode::fixedUpdate`. Rendering smooths the results between ticks.

Pause freezes the simulation. Stop destroys the bodies and restores the scene as you
authored it. Component data never holds Jolt's own handles.
