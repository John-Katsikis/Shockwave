# Animation

Shockwave plays the skeletal animation that comes inside your model files: a character's
walk, run and idle, a creature's attack, a door's swing. You add an Animator to the
model in your scene, pick a clip, and it plays. Game code switches clips with a smooth
cross-fade, and can find joints to hold things in a hand.

Clips are sampled and blended by [ozz-animation](https://github.com/guillaumeblanc/ozz-animation)
on every CPU core, and skinned meshes are bent on the GPU, so a crowd costs little.

## Try it

The sample project has three foxes, each playing a different clip of the same model:

```sh
./build/bin/editor Games/Sample/Scenes/animation.swscene
```

Select a fox and look at its Animator in the Inspector. Pick another clip, drag Scrub to
pose it, or press Play to watch them run. View > Preview animations plays them without
starting the game.

## Animate a model

1. Put a model with animation in your project's `Content/` folder. glTF and GLB work best;
   FBX, Collada and .blend files (through Blender) work too. See [Assets](Assets.md).
2. Drag it from the Content panel into the viewport. A model with clips gets an Animator
   right away, playing its first clip.
3. In the Inspector, use Pick clip to choose a clip, and Scrub to see any moment of it.

A model that is already in the scene without an Animator shows "Add Animator" under its
ModelRenderer when it has clips.

The same thing in a scene file:

```text
entity "Fox"
  Transform
    scale 0.012 0.012 0.012
  ModelRenderer
    model models/Fox.glb
  Animator
    clip Walk
end
```

To see a model's clips and how long they are:

```sh
./build/bin/player --animations                  # every animated model in Content
./build/bin/player --animations models/Fox.glb   # just this one
```

```text
models/Fox.glb: 27 nodes, 1 skinned mesh (up to 24 joints each), 3 clips
  clip "Survey" 3.417 s, 20 animated nodes
  clip "Walk" 0.708 s, 20 animated nodes
  clip "Run" 1.158 s, 20 animated nodes
```

## Animator fields

The Animator plays clips of the model in the same entity's ModelRenderer.

| Field | Default | What it does |
|---|---|---|
| `clip` | empty | The clip's name, exactly as in the model. Empty plays the first clip. |
| `playing` | on | Off holds the current pose. |
| `loop` | on | Starts again at the end. Off stops on the last frame. |
| `speed` | 1 | 2 plays twice as fast, 0.5 half as fast, a negative speed plays backwards. |
| `time` | 0 | Seconds into the clip to start from. The editor shows this pose while you edit. |
| `blend_time` | 0.2 | Seconds to cross-fade from the old clip when `clip` changes. 0 switches at once. |

Changing `clip` while the game runs, from code or the Inspector, cross-fades to the new
clip from its start. Changing `time` jumps there.

## Control animation from game code

Game code reaches animation through `game.animation()`:

```cpp
#include <Shockwave/Game.h>

void frame(shockwave::GameContext& game, double) override {
    auto hero = game.find("Hero");
    const bool moving = game.input().isActionDown("move");
    game.animation().play(hero, moving ? "Run" : "Idle");   // cross-fades over blend_time

    if (game.input().wasActionPressed("jump")) game.animation().play(hero, "Jump", 0.1f);
}
```

| Call | What it does |
|---|---|
| `play(entity, "Run")` | Cross-fades to the clip over the Animator's `blend_time` and plays it from the start. Already playing it: nothing changes. |
| `play(entity, "Jump", 0.1f)` | The same, over 0.1 seconds. |
| `stop(entity)` | Holds the current pose. |
| `finished(entity)` | True when a clip that doesn't loop has reached its end. |
| `clip(entity)`, `time(entity)`, `duration(entity)` | The clip playing, how far into it, and its length in seconds. |
| `clips(entity)` | The names of every clip the model has. |
| `jointTransform(entity, "hand_r", m)` | Where a joint is in the world this frame, as a matrix. False if there's no such joint. |

Every call returns false, or does nothing, for an entity without an Animator or a clip
name the model doesn't have. You can also change the Animator's fields directly, for
example `hero.get<shockwave::Animator>()->speed = 1.5f;`.

To hold a sword in a hand, copy the joint's matrix to the sword each frame:

```cpp
glm::mat4 hand;
if (game.animation().jointTransform(hero, "hand_r", hand))
    *sword.get<shockwave::Transform>() = shockwave::Transform::fromMatrix(hand);
```

Joint names are the node names in the model file. Turn on View > Skeletons in the editor
to see the joints of the selected model.

## Check animations for mistakes

`player --check` checks every Animator in every scene:

```sh
./build/bin/player --check
```

It reports, with the scene and entity:

- an Animator without a ModelRenderer on the same entity;
- a model with no clips;
- a clip name the model doesn't have, listing the ones it does;
- a negative `time` or `blend_time`;
- a model too big to animate (more than 1024 nodes).

A `time` past the end of the clip is printed as a warning and doesn't fail the check. The
Inspector shows the same problems under the Animator.

## See it from the command line

Render a scene in its starting poses, the way an AI tool or CI would check it. Each
Animator shows its `time`:

```sh
./build/bin/player --scene Games/Sample/Scenes/animation.swscene --screenshot foxes.png --frames 1
```

With more frames the clips play on before the picture is taken. `--benchmark` reports
what animation costs:

```text
  pass skinning                 0.360 ms
animation cpu: 0.023 ms last frame (3 animators, 81 joints; GPU: the skinning pass)
```

The editor's Stats window shows the same numbers under Animation.

## Performance

- Each Animator is sampled and blended on the job system, so all CPU cores share the
  work. On a 2-worker test machine, 200 foxes took about 0.6 ms a frame.
- Skinned meshes are bent by one GPU compute pass at the start of the frame. The scene
  and every shadow map then draw the bent vertices, so a character is skinned once
  however many shadows it casts.
- Animation advances with the real frame time, not the fixed game tick, so it stays
  smooth at any frame rate you choose.
- ozz-animation stores clips compactly and caches where it is in each clip, so playing
  forward is cheap.

## Limits

- Up to 4 joints per vertex. Assimp keeps the 4 strongest when a model has more.
- Up to 1024 nodes per model, and 65535 joints per skinned mesh.
- Morph targets (blend shapes, often used for faces) aren't played yet. The model loads
  and `player --check` warns at import.
- No root motion: a clip that walks forward in the file moves the mesh, not the entity's
  Transform. Move the entity from game code.
- No animation graphs, layers per body part, IK or additive clips yet. One clip plays at
  a time, with a cross-fade between two when it changes.
- Picking in the editor and the outline use the model's rest pose, not the current one.

## How it works

For people curious about the internals:

- **Import** (Assets module, `Model.cpp`): Assimp reads the skin weights, each joint's
  inverse bind matrix and the clips. `ModelData` keeps them as plain data, with no
  ozz-animation types, so other animation runtimes could use the same data.
- **Rig** (Animation module, `Rig.cpp`): when a model first plays, its nodes become an
  ozz skeleton, joint i being model node i, and each clip becomes an ozz animation. The
  rig is shared by every entity using that model and rebuilt when the model reloads.
- **Playing** (`AnimationWorld.cpp`): once per frame, each Animator's clip is sampled,
  cross-faded with the previous clip if one is fading out, and turned into a matrix per
  node. The result goes in a `ModelPose` component (Engine/Pose.h), which is never saved.
- **Drawing** (`renderWorld` in `Systems.cpp`): a skinned mesh is drawn with
  `Renderer::drawSkinnedMesh` and one matrix per joint. Other meshes in an animated model
  follow their node's animated matrix.
- **Skinning** (Vulkan renderer, `RendererSkinning.cpp`, `skinning.comp`): each skinned
  draw gets a slice of a per-frame vertex buffer. A compute pass writes the posed
  vertices there, and the draw points at them. A renderer plugin that doesn't skin can
  treat `drawSkinnedMesh` as a plain draw.
- **Editor and Play**: the editor keeps an AnimationWorld for edit mode (still, or
  playing with Preview animations). Each Play session has its own, and Stop puts every
  Animator back as it was.
