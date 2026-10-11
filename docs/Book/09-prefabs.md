# 9. Prefabs: build once, spawn many

**In this chapter:** turning the orb into a prefab, placing copies, spawning them from
code, and collecting them.

## What a prefab is

Copying the same orb by hand ten times is tedious, and changing all ten later is worse.
A **prefab** is a reusable template: a group of entities saved in its own file. Every
copy follows the prefab, so changing the prefab changes every copy, in every scene, at
once. Prefabs can even contain other prefabs.

If you know Unity prefabs or Unreal Blueprint actors, it's the same idea.

## Make the orb a prefab

The easy way is in the editor: right-click "Orb 1" in the Hierarchy and choose **Make
prefab**. The editor writes `Content/Prefabs/Orb 1.swprefab` and turns the entity into a
copy of it.

Prefabs are plain text too, so let's write it by hand to see what's inside. Create
`Games/OrbHunt/Content/Prefabs/Orb.swprefab`:

```text
shockwave_prefab 1

entity "Orb"
  Transform
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
  Orb
    points 1
end
```

It looks like a scene with a different first line. The first entity is the **root**.
A bigger prefab would list more entities after it, and any without a `Parent` hang
under the root.

## Place a copy in the scene

Delete "Orb 1" and "Orb 2" from `arena.swscene`, and put one copy in their place:

```text
entity "First orb"
  Prefab
    path "Prefabs/Orb.swprefab"
  Transform
    position 4 1 -3
    scale 0.5 0.5 0.5
end
```

A copy lists only what's different about *this* copy, usually its Transform.
Everything else comes from the prefab. In the editor you'd drag the prefab from the
Content panel into the viewport instead. Copies show in the accent color in the
Hierarchy, with **Prefab** on the right.

To change every orb at once, double-click the prefab in Content, edit it, and save.
Every copy updates straight away. Saving the file in a text editor works too: the
editor checks prefab files twice a second.

## Spawn orbs from code

Placing copies by hand is good for level design. For Orb Hunt, we want orbs to appear at
random places, and a new one to appear each time you collect one. That's `game.spawn`.

Add `#include <random>` under the first include, then add these to `OrbHunt`:

```cpp
    // In beginPlay, after finding the player:
    for (int i = 0; i < 10; ++i) spawnOrb(game);
```

```cpp
    // At the end of fixedUpdate:
    collectOrbs(game);
```

```cpp
private:
    // A new orb somewhere in the arena.
    void spawnOrb(GameContext& game) {
        std::uniform_real_distribution<float> spread(-10.0f, 10.0f);
        game.spawn("Prefabs/Orb.swprefab", {spread(m_random), 1.0f, spread(m_random)});
    }

    // Collect every orb within a meter of the player, and spawn a new one for each.
    void collectOrbs(GameContext& game) {
        const glm::vec3 feet = m_player.get<Transform>().position;
        std::vector<Entity> collected;
        game.world.each<Transform, Orb>([&](Entity e, Transform& transform, Orb& orb) {
            const glm::vec2 offset(transform.position.x - feet.x, transform.position.z - feet.z);
            if (glm::length(offset) < 1.0f) {
                collected.push_back(e);
                m_score += static_cast<int>(orb.points);
            }
        });
        for (Entity e : collected) {
            game.world.destroy(e);
            spawnOrb(game);
        }
        if (!collected.empty()) SW_LOG(LogGame, Info, "Score: {}", m_score);
    }

    EntityRef m_player;
    std::mt19937 m_random{std::random_device{}()};
    int m_score = 0;
```

(`m_player` was already there; keep just one.) Rebuild and play. Ten orbs appear, and
walking into one makes it vanish, a new one appear elsewhere, and the Console print
your score.

A few things worth noticing:

- **Collect first, destroy after.** We never destroy an entity while
  `world.each` is walking the list. Changing a list while you're looping over it is a
  classic bug. The `collected` vector avoids it.
- **This runs in a hook, not a system.** Spawning creates entities and reads prefab
  files, so it belongs on the main thread (chapter 6). The hook still runs in the fixed
  tick, so collection is steady.
- **Distance on the ground.** We compare only X and Z, so an orb bobbing up and down
  doesn't change whether you can reach it.

> **Under the hood.** Prefabs live in `Source/Runtime/Engine/Private/Prefab.cpp`. A
> scene entity with a `Prefab` component is expanded into the prefab's entities when
> the scene loads; only the copy's own changes are written back out when it's saved.
> `game.spawn` calls `spawnPrefab`. The first spawn of a prefab reads the file, and
> later spawns copy from memory. Physics picks up colliders on spawned copies on the
> next tick.

## Recap

- A prefab is a `.swprefab` file in Content: a template of one or more entities.
- A scene places a copy with a `Prefab` component plus its own changes.
- `game.spawn("Prefabs/X.swprefab", position)` makes a copy from code.
- Don't change the World while looping over it. Collect, then change.

Next: [Sound](10-sound.md).
