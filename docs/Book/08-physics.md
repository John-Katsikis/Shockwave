# 8. Physics: solid ground and a walking player

**In this chapter:** colliders and rigid bodies, the character controller, and moving
the player properly, inside the fixed tick.

## Three kinds of physical things

Shockwave's physics is the Jolt physics engine. You set it up with components, the
same way as everything else:

| You want | Give the entity |
|---|---|
| Something solid that never moves: a floor, a wall | `Transform` + `Collider` |
| Something that falls, bounces and can be pushed | `Transform` + `Collider` + `RigidBody` (motion `Dynamic`) |
| Something your code moves, like a lift | `Transform` + `Collider` + `RigidBody` (motion `Kinematic`) |
| A character that walks, climbs steps and jumps | `Transform` + `CharacterController` |

A **Collider** is the invisible shape physics uses: a box, sphere, capsule, or the
triangles of a model. It doesn't have to match what's drawn; a simple box around a
detailed statue is usually best. The collider is scaled by the entity's `Transform`,
so a cube collider on the flattened floor is flattened too.

A **RigidBody** adds mass, friction, bounciness (`restitution`) and the rest.
[Physics](../Physics.md) lists them all.

## Make the floor solid

Add a `Collider` to the Floor in `arena.swscene`:

```text
entity "Floor"
  Transform
    position 0 -0.25 0
    scale 24 0.5 24
  MeshRenderer
    mesh cube
    texture grid
    tint 0.3 0.35 0.45 1
  Collider
    shape Box
    half_extents 0.5 0.5 0.5
end
```

`half_extents 0.5 0.5 0.5` is a 1 x 1 x 1 box, the same as the cube mesh. The
Transform's scale stretches both.

## Add something to push

Add a crate that falls from the sky when Play starts:

```text
entity "Crate"
  Transform
    position 3 4 3
    rotation 15 30 0
  MeshRenderer
    mesh cube
    texture panel
    tint 1 0.6 0.3 1
  Collider
    shape Box
  RigidBody
    motion Dynamic
    mass 5
    friction 0.6
end
```

## Make the player a character

Give the Player a `CharacterController`:

```text
entity "Player"
  Transform
    position 0 1 0
  CharacterController
    radius 0.35
    half_height 0.5
    jump_speed 6
end
```

The controller is an upright capsule with its feet at the Transform's position. It
handles gravity, slopes, stepping up small ledges and pushing dynamic bodies.

Now you can see why chapter 4 split the player into two entities. Physics owns the
Player's Transform: every tick it writes where the capsule ended up. The visible body is
a child, so it simply follows. Keep that pattern for anything physics drives: physics on
the parent, looks on the children.

## Drive it from code

You don't move a character by changing its Transform. You tell it how you'd *like* to
move, and physics works out where it can actually go. The controller has four runtime
fields:

| Field | Who writes it | Meaning |
|---|---|---|
| `desiredVelocity` | your code | how fast and which way to walk, in meters per second |
| `jumpRequested` | your code | set it to true to jump once |
| `velocity` | physics | how it's actually moving |
| `grounded` | physics | whether it's standing on something |

Replace the `frame` hook in `OrbHunt.cpp` with these two hooks:

```cpp
    // Every fixed tick: say where the player wants to go.
    void fixedUpdate(GameContext& game, double) override {
        if (!m_player) return;
        Input& input = game.input();
        const float right = input.axis("move_left", "move_right");
        const float forward = input.axis("move_back", "move_forward");
        const float speed = m_player.get<Runner>().speed;
        m_player.get<CharacterController>().desiredVelocity = glm::vec3(right, 0.0f, -forward) * speed;
    }

    // Every frame: catch the jump press the moment it happens.
    void frame(GameContext& game, double) override {
        if (m_player && game.input().wasActionPressed("jump"))
            m_player.get<CharacterController>().jumpRequested = true;
    }
```

Why the split? Holding a direction is a *state*: reading it once per tick is exactly
right. Pressing jump is an *event*: `wasActionPressed` is true only on the frame the
button went down, and frames are where input is read. Setting `jumpRequested` there
means the next tick picks it up.

Rebuild and play. The crate drops and settles, the player falls onto the floor, walks,
jumps, and shoves the crate around. Walk off the edge and you'll fall forever, which is
a good reason to add walls (try it: four flattened cubes with Colliders).

## Changing gravity

Put one `PhysicsSettings` component on any entity, such as the Environment:

```text
  PhysicsSettings
    gravity 0 -15 0
    collision_steps 2
```

Stronger gravity makes jumps snappier. `collision_steps` splits each tick into smaller
physics steps, which costs more but keeps fast objects from passing through each other.

> **Under the hood.** Physics is the `Physics` module
> (`Source/Runtime/Physics`). When Play starts, the PlaySession adds a `PhysicsWorld`
> subsystem, calls your `beginPlay`, and then creates a Jolt body for every entity with
> a Collider or CharacterController. Each tick, after your systems run,
> `physics()->step(dt)` advances Jolt and writes the results back into the
> Transforms. On Stop, the PhysicsWorld is destroyed and the authored scene is
> restored, so the crate is back in the sky next time. Raycasts, contact events and
> triggers (`RigidBody` with `sensor 1`) are in [Physics](../Physics.md).

## Recap

- `Collider` gives a shape; `RigidBody` makes it move under physics; a
  `CharacterController` walks.
- Physics owns the Transform of what it simulates. Put visuals on a child.
- Set `desiredVelocity` every tick and `jumpRequested` on the press.
- Steady state goes in `fixedUpdate`; one-off presses in `frame`.

Next: [Prefabs: build once, spawn many](09-prefabs.md).
