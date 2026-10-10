# Jolt physics

Shockwave builds Jolt 5.6.0 through CMake. Editor Play and the standalone player use
one `PhysicsWorld` per game session, running on Shockwave's existing worker pool.
The default fixed rate is 60 Hz. Each tick runs scheduled gameplay systems, physics,
then `Gameplay::onPhysicsStep`. Rendering interpolates the resulting transforms.
Pause freezes the simulation; Stop destroys its bodies and restores the authored ECS.

## Authoring

Add components through the Inspector's Component menu and save normally:

- `Transform` + `Collider` is static collision geometry.
- Add `RigidBody` and select Dynamic for gravity-driven objects, or Kinematic for
  objects moved by gameplay. Static bodies remain stationary until explicitly moved.
- Shapes are Box (half extents), Sphere (radius), Capsule (cylinder half height plus
  spherical caps), ConvexModel, and TriangleModel. Offset is in local coordinates.
- Imported shapes require `ModelRenderer`. Collision includes all imported mesh/node
  instances. A convex model becomes one hull, filling cavities; triangle models are
  static only. Reloading a model rebuilds its collision shape.
- `RigidBody` supplies mass, friction, restitution, damping, gravity multiplier,
  initial velocities, sensor, and continuous collision detection. Continuous mode
  applies to dynamic non-sensor bodies. Angular velocity is radians per second.
- A sensor reports overlap events without physical response. Static sensors detect
  moving bodies; kinematic sensors also detect static geometry. Sensors do not report
  overlaps with other sensors. Sleeping bodies retain sensor overlap membership.
- Put one `PhysicsSettings` on a scene entity to set gravity and 1–8 collision substeps.
  Without it, gravity is `(0, -9.81, 0)` and there is one substep per fixed tick.

Physics settings are reflected, themed automatically, and serialized in `.swscene`.
Invalid bodies are skipped with entity diagnostics in the active Inspector and
Renderer panel. The player rejects initial physics diagnostics. A disabled Collider,
RigidBody or CharacterController removes that entity's runtime physics until enabled.

Boxes and imported meshes support nonuniform scale. Spheres, capsules, and characters
require uniform scale. Dynamic bodies and characters require uniformly scaled parent
hierarchies; sheared transforms are unsupported. Character scale must be positive.
Dynamic results convert world position/rotation back to local transforms, preserving
authored scale. Editing a dynamic Transform during Play teleports the body; changing
physical settings or collision geometry rebuilds it. Kinematic Transform changes
become velocity-based targets on the next tick.

## Gameplay

Include `shockwave/Gameplay.h` and `shockwave/Physics.h`. `ctx.physics` is valid from
`onStart` through `onStop`. The session synchronizes new bodies after `onStart`; call
`ctx.physics->synchronize()` inside that hook if you need immediate queries or impulses.
Scheduled systems can move kinematic Transforms or set character intent before physics.

```cpp
void onPhysicsStep(shockwave::GameplayContext& ctx, double dt) override {
    for (const auto& event : ctx.physics->contacts()) {
        if (event.sensor && event.type == shockwave::ContactEvent::Type::Begin) {
            // event.a is the sensor; event.b is the overlapping entity.
            // Activate a checkpoint, door, or other gameplay behavior here.
        }
    }
    // Direction is normalized by raycast; hit distance is in world units.
    if (auto hit = ctx.physics->raycast({0, 5, 0}, {0, -1, 0}, 10)) {
        // hit->entity, point, normal and distance identify the closest hit.
    }
    (void)dt;
}
```

`addImpulse`, `addForce`, and `setVelocity` act on dynamic bodies and return false for
invalid, destroyed, static, kinematic, or character entities. Impulses change momentum
immediately; forces apply to the next step. Kinematic bodies are driven by Transform
targets. Raycasts can ignore an entity, include sensors (off by default), and filter
collision categories. Each body selects a category 0–15 and a 16-bit mask; two bodies
collide only when both masks include the other's category. Query masks select categories.

`contacts()` is the current tick's Begin/Stay/End list, consumed after simulation on
the main thread. Ordinary rigid-body events follow Jolt contact lifecycle, including
contact removal when bodies sleep. Sensor events instead track overlap across sleeping
bodies, suitable for gameplay entry/exit logic. End events may reference destroyed
entities: check `world.alive()` before accessing their components. Contact normals point
from a toward b. No gameplay/ECS mutation runs inside Jolt's worker contact callbacks.

Physics stepping and synchronization belong to the main thread between schedule runs.
Queries, impulses, forces, and velocity calls use Jolt locking and may run in gameplay
workers between steps. Do not mutate the World concurrently with synchronization or
stepping. Component snapshots never contain native Jolt handles.

## Characters

Add `CharacterController` with a Transform, without Collider or RigidBody. It supplies
an upright Y-up capsule, feet at Transform.position, gravity, slopes, ground following,
stair stepping, jumping and pushing dynamic bodies. Attach a separate child visual mesh.
Gameplay sets `desiredVelocity` in world-space horizontal units per second and sets
`jumpRequested` for one tick. Physics writes `velocity` and `grounded`. These runtime
fields are not saved. The controller has a queryable inner body; it is not a full
animated player prefab and does not bind keyboard, gamepad, or camera automatically.

## Demo and checks

```sh
./build/bin/editor scenes/physics.swscene --play
ctest --test-dir build --output-on-failure
```

The demo contains falling boxes and spheres, a moving platform, a trigger, imported
convex/triangle collision, and a character. To run it in the player, point a project
manifest's startup scene at `scenes/physics.swscene`.

The CPU physics tests cover contacts, sleeping sensor overlaps, filtering, forces,
kinematic targets, raycasts, characters/stairs, imported geometry and reload, parenting,
scene persistence, and shared session pause/restart/restore. The editor and player also
have bounded `--frames N` smoke runs. This milestone does not implement joints, vehicles,
ragdolls, character animation, a collision debugger, or persisted collision cooking.
