# Prefabs

A prefab is a group of entities you build once and reuse: a street lamp, an enemy, a
crate stack. Each copy you place follows the prefab, so changing the prefab changes
every copy at once. If you know Unity prefabs or Unreal Blueprint actors, it's the same
idea. Prefabs can contain other prefabs.

Prefabs are plain text `.swprefab` files in your project's `Content/` folder, so people,
scripts and AI tools can all write them. The sample project has three in
`Games/Sample/Content/Prefabs/`, used by `Games/Sample/Scenes/prefabs.swscene`.

## Make a prefab in the editor

1. Build the thing in a scene: one entity with others under it in the Hierarchy.
2. Right-click the top entity and choose **Make prefab**.

The editor writes `Content/Prefabs/<Name>.swprefab` and turns your entity into a copy
of it, right where it was. The prefab's root sits at the origin, so the copy's
Transform says where it stands.

## Place a copy

- Drag a prefab from the **Content** panel into the viewport, or
- right-click it in the Content panel and choose **Place in front of the camera**.

Copies show in the accent color in the Hierarchy, with **Prefab** on the right. The
entities under a copy are its *parts*. They come from the prefab and are shown a little
fainter.

Clicking a copy in the viewport selects the whole copy. Click again to select the part
you clicked.

## Change one copy

Select the copy and edit it in the Inspector as usual. Changes to the copy's own
components (its Transform, its MeshRenderer tint and so on) belong to that copy. They
stay when the prefab changes, and only they are saved in the scene.

You can also put new entities under a copy, for example a sign on one crate stack.
They are saved in the scene as ordinary entities.

Parts can't be changed on their own: the Inspector shows them, but greyed out. To
change a part, edit the prefab, or unpack the copy.

## Edit a prefab

Double-click the prefab in the Content panel, or use **Edit prefab** in the Inspector
or the Hierarchy's right-click menu. The prefab opens like a scene, with **PREFAB** in
the command bar and **Editing a prefab** above the Hierarchy.

- The first entity in the Hierarchy is the root.
- Save with **Ctrl+S**. Every copy, in every scene, updates straight away.
- **Back to** *scene* at the top of the Hierarchy returns to the scene you came from.

Prefabs also update when you save the file in a text editor. The editor and the
player both check for changes twice a second.

## Send a copy's changes to the prefab

Select the copy and press **Apply** in the Inspector, or choose **Apply to prefab**
from the Hierarchy's right-click menu. This writes into the prefab:

- the copy's components and their values, and
- any entities you added under the copy.

It doesn't write where the copy stands or its name, so other copies stay where they are.

## Turn a copy back into ordinary entities

Choose **Unpack** in the Inspector, or **Unpack prefab** from the right-click menu. The
copy and its parts become ordinary entities that no longer follow the prefab. Prefab
copies nested inside it stay copies.

## Spawn copies from game code

Use `spawn` on the `GameContext` your GameMode gets:

```cpp
#include <Shockwave/Game.h>
using namespace shockwave;

class MyGameMode : public GameMode {
    void beginPlay(GameContext& game) override {
        // At the prefab's own place, or at a position.
        EntityRef lamp = game.spawn("Prefabs/Street lamp.swprefab");
        EntityRef crate = game.spawn("Prefabs/Crate stack.swprefab", {2, 0.5f, 0});
        if (!crate) SW_LOG(LogGame, Warning, "no crate prefab");
    }
};
```

`spawn` returns an `EntityRef` to the copy's root, or an empty one (false) if the prefab
can't be loaded. Physics picks up colliders on spawned copies on the next tick.

Spawning creates entities, so call it from GameMode hooks (`beginPlay`, `fixedUpdate`,
`frame`), not from inside a system. The first spawn of a prefab reads its file. Later
spawns copy from memory. To read a file early, during a loading screen say, call
`prefabs().load("Prefabs/Enemy.swprefab")`.

The lower-level functions are in `Engine/Prefab.h`: `spawnPrefab`, `unpackPrefab`,
`prefabRootOf` and more.

## Write prefabs as text

A prefab file looks like a scene with its own header line. The first entity is the
root. Other entities name their parent with `Parent`. Any entity without a `Parent`
is attached to the root. Transforms under the root are relative to it.

```text
# Content/Prefabs/Street lamp.swprefab
shockwave_prefab 1

entity "Street lamp"
  Transform
end

entity "Pole"
  Parent
    entity "Street lamp"
  Transform
    position 0 1.5 0
    scale 0.12 3 0.12
  MeshRenderer
    mesh cube
end

entity "Glow"
  Parent
    entity "Street lamp"
  Transform
    position 0 2.85 0
  Light
    type Point
    intensity 5
end
```

To place a copy in a scene, or inside another prefab, give an entity a `Prefab`
component with the file's path inside `Content/`. Then list only what this copy
changes on the prefab's root. Everything you leave out comes from the prefab.

```text
entity "Lamp by the door"
  Prefab
    path "Prefabs/Street lamp.swprefab"
  Transform
    position 4 0 -2
end
```

A few rules:

- The parts aren't written in the scene. Loading the scene makes them again from the
  prefab. When the editor saves, it writes only the fields that differ from the prefab.
- New entities can go under a copy. Their `Parent` names the copy, or a part by its name.
- A prefab can contain copies of other prefabs, as deep as you like, but not a copy of
  itself. A prefab whose root is a copy of another prefab is a *variant*: it starts from
  that prefab and changes it.
- Keep `Environment` in scenes, not prefabs: it sets the look of the whole scene.

The [scene schema](SceneSchema.md) lists every component and field, and the prefabs in
the project. Regenerate it with `build/bin/player --schema docs/SceneSchema.md`.

## Check prefabs for mistakes

```sh
build/bin/player --check
```

This checks every scene and every `.swprefab` in `Content/`. It prints one line per
problem, like `Content/Prefabs/Street lamp.swprefab:12: unknown field 'colour' in Light
skipped`, and an `ok` line for each good prefab. It reports:

- prefab files that are missing or can't be read, with the scene line that uses them;
- a prefab that contains itself, with the chain (A -> B -> A);
- unknown components and fields, values that don't parse or are out of range;
- a `Prefab` component with no path;
- a Light without a Transform, an Environment inside a prefab, and a root without a
  Transform (copies couldn't be placed).

With scene files named on the command line, it checks the prefabs those scenes use.

To see a scene with its prefabs as the game draws it:

```sh
build/bin/player --scene Games/Sample/Scenes/prefabs.swscene --screenshot prefabs.png
```

## Limits

- A copy can't remove a component its prefab's root has. If you remove one in the
  editor, it comes back when the scene loads. Unpack the copy instead.
- Changes to a part can't be kept per copy. Edit the prefab, or unpack the copy.
- Prefabs don't update during Play in the editor. They catch up when you press Stop.
  In the player, they update while the game runs.
- Entity links from outside a copy to one of its parts work by name, so they're only
  reliable when that name is unique in the scene.

## How it works

`Engine/Prefab.h` has the details. In short:

- `prefabs()` is a library that loads each `.swprefab` once and caches it. It reloads a
  file when it changes on disk, along with every prefab that uses it.
- Loading a scene gives each entity with a `Prefab` component the prefab root's
  components, then puts the scene's own values on top. The rest of the prefab is created
  under it, each part marked with `PrefabPart`.
- `updatePrefabInstances` remakes copies whose prefab was reloaded. Before it does, it
  compares each copy's root with the old prefab, so the copy's own changes survive.
- Saving compares each copy's root with its prefab and writes only the differences.
  Parts are skipped.
- Game code hot reload saves the scene with every entity in full (`SceneFileHooks::exact`),
  so entity handles survive the reload.
