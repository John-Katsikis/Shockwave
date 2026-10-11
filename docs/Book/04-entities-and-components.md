# 4. Entities and components

**In this chapter:** the three words that explain how a Shockwave world is built, and
the first scene for Orb Hunt.

## Three words: entity, component, system

Look around a room. There's a lamp, a chair, a window. In Shockwave, each of those is an
**entity**. An entity on its own is nothing but an ID number. It has no shape, no
position, no behavior.

What makes the lamp a lamp is the **components** you attach to it:

- a `Transform` says where it is, which way it faces and how big it is;
- a `MeshRenderer` says what shape to draw, and in what color;
- a `Light` says it gives off light.

Components are **plain data**: a struct with a few fields and no code. Here's
`Transform`, from `Source/Runtime/Engine/Public/Engine/Components.h`:

```cpp
struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};   // files and the editor show degrees
    glm::vec3 scale{1.0f};
};
```

So who makes things *move*? **Systems**. A system is a function that runs on every
entity that has a certain set of components. The built-in `Spin` system, for example,
looks for every entity with both a `Transform` and a `Spin` component, and turns it a
little each update.

| Word | Is | Example |
|---|---|---|
| Entity | An ID. A thing in the world. | "Crate" |
| Component | Plain data attached to an entity. | `Transform`, `MeshRenderer`, `Spin` |
| System | A function over every entity with certain components. | "turn everything that has Spin" |

That's the whole idea of an **ECS** (entity component system).

### Why build it this way?

If you've used Unity, you'd give a crate a script with an `Update()` method. Each crate
updates itself, one after another, on one CPU core.

In an ECS, there's no `Update()` on the crate. Instead, *one* system updates *all* the
crates at once. That has two big benefits:

1. **Speed.** All the `Spin` data sits together in memory, so the CPU zips through it.
   And because a system says exactly which components it touches, Shockwave can run
   different systems at the same time on different cores, and split one system's
   entities across cores too. Chapter 6 shows how.
2. **Simplicity.** Data is just data. Saving a game, copying an entity, undoing an edit
   or reloading code all become "copy these structs", with no hidden state inside
   objects.

## The World

All the entities and their components live in a **World**. When you open a scene in the
editor, you're looking at a World. When you press Play, the game runs on that same
World. Stop puts the World back the way it was.

## Scenes are text

A scene file is just a list of entities and their components. Here's an entity from the
sample's `Scenes/main.swscene`:

```text
entity "Ball"
  Transform
    position 2.2 0.75 0.4
    scale 1.5 1.5 1.5
  MeshRenderer
    mesh sphere
    texture checker
    tint 0.35 0.65 1 1
  Bob
    amplitude 0.4
    frequency 0.5
end
```

Read it like a sentence: *there's an entity called Ball; it's at (2.2, 0.75, 0.4) and
one and a half times normal size; it's drawn as a sphere with a checker texture, tinted
blue; and it bobs up and down.*

The rules are simple:

- `entity "Name"` starts an entity and `end` finishes it.
- Component names are indented two spaces, their fields four.
- Leave out any field to keep its default. Leave out a component to not have it.
- +Y is up, -Z is forward, and one unit is one meter.

Every component and field is listed in [SceneSchema.md](../SceneSchema.md), which the
engine generates from its own code, so it's always right.

## Parents and children

An entity can have a `Parent`. Then its `Transform` is measured from the parent, and it
moves when the parent moves:

```text
entity "Moon"
  Transform
    position 1.8 0.4 0
  Parent
    entity "Small crate"
end
```

The moon orbits because its parent spins. In the editor's Hierarchy, children show
indented under their parent, and you drag one entity onto another to attach it.

## Orb Hunt's arena

Create `Games/OrbHunt/Scenes/arena.swscene`:

```text
shockwave_scene 2

entity "Environment"
  Environment
    sky_color 0.05 0.07 0.12 1
end

entity "Floor"
  Transform
    position 0 -0.25 0
    scale 24 0.5 24
  MeshRenderer
    mesh cube
    texture grid
    tint 0.3 0.35 0.45 1
end

entity "Sun"
  Transform
    rotation -55 30 0
  Light
    type Directional
    intensity 2.5
end

entity "Player"
  Transform
    position 0 1 0
end

entity "Player body"
  Transform
    position 0 0.85 0
    scale 0.7 1.7 0.7
  Parent
    entity "Player"
  MeshRenderer
    mesh sphere
    texture white
    tint 1 0.55 0.2 1
end

entity "Camera"
  Transform
    position 0 14 14
    rotation -45 0 0
  Camera
    primary 1
end
```

A few things to notice:

- The **Environment** sets the sky color. There's one per scene.
- The **Floor** is a cube squashed flat: 24 meters wide, half a meter thick.
- The **Player** is an empty entity with only a position. Its visible body is a
  *child*, a stretched sphere. Chapter 8 explains why the two are split.
- The **Camera** has `primary 1`. Every playable scene needs exactly one primary
  camera, because that's what the game looks through.

Open it:

```sh
./build/bin/editor Games/OrbHunt
```

The editor will say the game module isn't built yet, and Play will be greyed out. That's
expected: we haven't written any code. You can still click around, select the Player
and look at its components.

## Recap

- An **entity** is an ID; **components** are plain data on it; **systems** are
  functions that process every entity with certain components.
- All entities live in a **World**. A scene file is a World written out as text.
- `Parent` makes one entity move with another.
- Every scene needs one `Camera` with `primary 1`.

Next: [Your first game code](05-first-game-code.md).
