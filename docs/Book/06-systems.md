# 6. Systems: work on every core

**In this chapter:** writing a system, how Shockwave decides which systems can run at
the same time, and the first orbs.

## The first orbs

Add two orbs to `Scenes/arena.swscene`, before the Camera:

```text
entity "Orb 1"
  Transform
    position 4 1 -3
    scale 0.5 0.5 0.5
  MeshRenderer
    mesh sphere
    texture white
    tint 0.2 0.9 1 1
    emissive 0.2 0.9 1 1
    emissive_intensity 2
  Bob
    amplitude 0.2
    frequency 0.8
end

entity "Orb 2"
  Transform
    position -5 1 2
    scale 0.5 0.5 0.5
  MeshRenderer
    mesh sphere
    texture white
    tint 0.2 0.9 1 1
    emissive 0.2 0.9 1 1
    emissive_intensity 2
  Bob
    amplitude 0.2
    frequency 0.8
    phase 1.5
end
```

`emissive` makes the surface give off its own light, so the orbs glow even in shadow.
`Bob` is a built-in component: during Play a built-in system floats every entity with a
`Bob` up and down. The two orbs bob out of step because of their different `phase`.

## Our own system: a shimmer

Let's make the orbs' glow pulse. First, a component to mark something as an orb. Add
this to `OrbHunt.cpp`, under `Runner`:

```cpp
// Something the player can collect.
struct Orb {
    float points = 1;          // score for collecting it
    float shimmerSpeed = 3;    // how fast its glow pulses
    float time = 0;            // runtime only
};
SW_COMPONENT(Orb, SW_FIELD(points, 1, 100), SW_FIELD(shimmerSpeed, 0, 20))
```

Then a system. Systems are added in the GameMode's `configureSystems` hook. Add this
inside `class OrbHunt`:

```cpp
    void configureSystems(GameContext&, Schedule& schedule) override {
        schedule.each<MeshRenderer, Orb>("Shimmer", [](const SystemContext& ctx, MeshRenderer& mesh, Orb& orb) {
            orb.time += ctx.dt;
            mesh.emissiveIntensity = 2.0f + std::sin(orb.time * orb.shimmerSpeed);
        });
    }
```

Finally, give each orb in the scene an `Orb` component, after its `Bob`:

```text
  Orb
    points 1
```

Rebuild and play. Both orbs now pulse.

Read the system out loud: *"every tick, for each entity that has a MeshRenderer and an
Orb, advance the orb's clock and set the glow from a sine wave."* You never loop over
entities yourself. The schedule does that.

## How systems run in parallel

Here's the clever part. Look at the angle brackets: `each<MeshRenderer, Orb>`. They tell
the schedule exactly which components this system touches:

- a type written plainly, like `MeshRenderer`, means **writes**;
- a type written `const`, like `const Spin`, means **only reads**.

From that, the schedule works out what's safe:

- Two systems can run **at the same time** if neither writes something the other reads
  or writes.
- Two systems that clash run **one after the other**, in the order you added them.

So with the built-in systems and ours:

| System | Reads | Writes |
|---|---|---|
| Spin | Spin | Transform |
| Bob | | Transform, Bob |
| Shimmer | | MeshRenderer, Orb |

Spin and Bob both write `Transform`, so they take turns. Shimmer touches neither, so it
runs alongside them on another core. The schedule groups systems into **stages**: all
systems in a stage run at once, and stages run in order. **Window > Stats** in the
editor shows the stages it built.

On top of that, one `each` system splits its entities into batches (256 by default) and
hands the batches to different cores. A thousand orbs would shimmer on all your cores
at once.

Because the order of clashing systems is always the order you added them, the result
never depends on which thread happened to be faster. The game behaves the same on every
run.

## The rules inside a system

A system runs on a worker thread, alongside others. That gives it three rules:

1. **Touch only what you declared.** If your system reads `Health`, put `Health` (or
   `const Health`) in the angle brackets. Undeclared access is a data race.
2. **Don't create or destroy entities directly.** Other systems are walking the same
   lists. Queue the change instead with `ctx.commands()`:

   ```cpp
   ctx.commands().destroy(entity);
   ctx.commands().add(entity, Stunned{});
   ```

   Queued commands are applied once every system has finished.
3. **Keep side effects for the hooks.** Playing a sound, changing the UI or spawning a
   prefab belongs in `beginPlay`, `fixedUpdate` or `frame`, which run on the main
   thread.

If you need the entity's ID inside a system, ask for it as the second parameter:
`[](const SystemContext& ctx, Entity e, MeshRenderer& mesh, Orb& orb) { ... }`.

## When to use a system, and when a hook

| Use a system for | Use a hook for |
|---|---|
| The same small job on many entities | One-off things: find the player, start music |
| Work that should use all cores | Anything touching sound, UI, files or spawning |
| Steady per-tick rules: movement, timers, regeneration | Reacting to input presses |

> **Under the hood.** `Schedule` is in `Source/Runtime/Engine/Public/Engine/Schedule.h`.
> `each<Ts...>` turns your lambda into a general system with an `Access` list of reads
> and writes. When the schedule first runs, `build()` sorts systems into stages from
> those lists. Each stage is handed to the job system (`Core/Jobs.h`), a pool with one
> worker thread per CPU core. `world.parallelEach` does the batching. Commands are kept
> per thread, so queuing one needs no lock.

## Recap

- A system is a lambda added with `schedule.each<Components...>("Name", ...)` in
  `configureSystems`.
- `const` types are reads; plain types are writes. The schedule runs non-clashing
  systems at once and splits entities across cores.
- In a system: touch only what you declared, and queue structural changes with
  `ctx.commands()`.

Next: [What happens in one frame](07-one-frame.md).
