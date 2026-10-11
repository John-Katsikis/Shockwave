# 12. Saving the game

**In this chapter:** what a save holds, saving and loading from code, and keeping score
across a load. This finishes Orb Hunt's code.

## What gets saved

A save holds the **whole running World**: every entity, with every component field you
listed in `SW_COMPONENT`. That's the payoff for keeping game state in components:

- The orbs' positions are in their Transforms. Saved.
- Each orb's `points` is a listed field. Saved.
- Each orb's `time` isn't listed. Not saved, and it doesn't matter: the shimmer just
  restarts.

A save file is an ordinary scene file with a short header, so you can open it, read it
and diff it.

What *isn't* in the World is state you keep in your GameMode, like `m_score`. For that
there are **values**: small named numbers or strings that ride along with the save.

## Bind the keys

Add to `Config/Input.swinput`:

```text
bind quick_save key F5
bind quick_load key F9
```

## Save and load

In `frame`, after the jump check:

```cpp
        if (game.input().wasActionPressed("quick_save")) game.saves().save("quicksave", "Quick save");
        if (game.input().wasActionPressed("quick_load")) game.saves().load("quicksave");
```

`save` copies the World instantly and writes the file in the background, so the game
doesn't hitch. `load` reads in the background too; the World is swapped at the start of
the next tick.

## Keep the score

Two more GameMode hooks run around saving:

```cpp
    // Just before a save copies the world.
    void beforeSave(GameContext& game) override { game.saves().set("score", m_score); }

    // Just after a load replaced the world.
    void afterLoad(GameContext& game) override {
        m_score = game.saves().getInt("score");
        game.ui().set("score", m_score);
    }
```

Rebuild, play, collect a few orbs, press **F5**, collect more, press **F9**. The orbs
jump back to where they were and the score goes back too.

`m_player` still works after a load. Entities come back with the same IDs they were
saved with, so an `EntityRef` you found in `beginPlay` still points at the player.

Saves go in the project's `Saved/SaveGames/` folder. **Window > Save Games** in the
editor lists them, and can load one or start Play from one. From a terminal:

```sh
./build/bin/player Games/OrbHunt --saves
```

## The finished game code

Here's all of `OrbHunt.cpp`, for reference:

```cpp
#include <Shockwave/Game.h>

#include <random>

using namespace shockwave;

// How fast the player runs.
struct Runner {
    float speed = 5.0f;   // meters per second
};
SW_COMPONENT(Runner, SW_FIELD(speed, 0, 20))

// Something the player can collect.
struct Orb {
    float points = 1;          // score for collecting it
    float shimmerSpeed = 3;    // how fast its glow pulses
    float time = 0;            // runtime only
};
SW_COMPONENT(Orb, SW_FIELD(points, 1, 100), SW_FIELD(shimmerSpeed, 0, 20))

class OrbHunt final : public GameMode {
public:
    void configureSystems(GameContext&, Schedule& schedule) override {
        schedule.each<MeshRenderer, Orb>("Shimmer", [](const SystemContext& ctx, MeshRenderer& mesh, Orb& orb) {
            orb.time += ctx.dt;
            mesh.emissiveIntensity = 2.0f + std::sin(orb.time * orb.shimmerSpeed);
        });
    }

    void beginPlay(GameContext& game) override {
        m_player = game.find("Player");
        if (m_player && !m_player.has<Runner>()) m_player.add(Runner{});
        for (int i = 0; i < 10; ++i) spawnOrb(game);

        m_score = 0;
        game.ui().set("score", m_score);
        game.ui().on("reset", [this, &game](const UIEvent&) {
            m_score = 0;
            game.ui().set("score", m_score);
        });
        SW_LOG(LogGame, Info, "Orb Hunt started");
    }

    void fixedUpdate(GameContext& game, double) override {
        if (!m_player) return;
        Input& input = game.input();
        const float right = input.axis("move_left", "move_right");
        const float forward = input.axis("move_back", "move_forward");
        const float speed = m_player.get<Runner>().speed;
        m_player.get<CharacterController>().desiredVelocity = glm::vec3(right, 0.0f, -forward) * speed;
        collectOrbs(game);
    }

    void frame(GameContext& game, double) override {
        if (m_player && game.input().wasActionPressed("jump"))
            m_player.get<CharacterController>().jumpRequested = true;
        if (game.input().wasActionPressed("quick_save")) game.saves().save("quicksave", "Quick save");
        if (game.input().wasActionPressed("quick_load")) game.saves().load("quicksave");
    }

    void beforeSave(GameContext& game) override { game.saves().set("score", m_score); }

    void afterLoad(GameContext& game) override {
        m_score = game.saves().getInt("score");
        game.ui().set("score", m_score);
    }

private:
    void spawnOrb(GameContext& game) {
        std::uniform_real_distribution<float> spread(-10.0f, 10.0f);
        game.spawn("Prefabs/Orb.swprefab", {spread(m_random), 1.0f, spread(m_random)});
    }

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
            game.audio().playClip("Audio/chime.wav", {.volume = 0.6f});
        }
        if (!collected.empty()) game.ui().set("score", m_score);
    }

    EntityRef m_player;
    std::mt19937 m_random{std::random_device{}()};
    int m_score = 0;
};

SW_GAME("orb_hunt", OrbHunt)
```

About 90 lines for a game with physics, spawning, sound, a HUD and save games. The
engine does the rest.

> **Under the hood.** Saving is part of `GameFramework` (`SaveGame.cpp`). `save` calls
> `beforeSave`, copies the World into a scene-format string on the main thread, and
> hands the writing to a worker. A load is parsed on a worker, then swapped in at the
> start of a tick by `PlaySession::applyLoadedSave`, which also rebuilds physics from
> the loaded components before calling `afterLoad`.

## Recap

- A save is the whole World, plus values you add.
- Keep game state in components and it's saved for free; use
  `game.saves().set/getInt` for the rest.
- `beforeSave` and `afterLoad` let your GameMode take part.

Next: [Drawing the picture: the renderer](13-rendering.md).
