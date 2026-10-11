# Tutorial: your first game in Shockwave

In about 20 minutes you'll make a small game from scratch: a cube you can drive around
with WASD or a gamepad, and make jump. Along the way you'll learn the four things every
Shockwave game is made of:

- a **project file** that says where everything is,
- **scenes**, which are plain text files you can edit in the editor or by hand,
- **components**, which are the data you attach to things in a scene,
- a **GameMode**, which holds your game's rules in C++.

You'll also see hot reload: you change your C++ while the game runs, and the change
shows up a second later without a restart.

You need Shockwave built once. If you haven't done that yet, follow "Get started" in
the [README](../README.md) first, then come back.

## 1. Look around the sample

Start the editor on the sample project that comes with Shockwave:

```sh
./build/bin/editor Games/Sample/Sample.swproject
```

Press **Ctrl+P** to play and again to stop. During Play the orange crate pulses, and
**Space** makes it flash. That's the sample's game code, in
`Games/Sample/Source/SampleGame.cpp`. Press **F1** any time to see the editor's controls.

Close the editor when you're done looking.

## 2. Make a project folder

Each game lives in its own folder. Make one next to the sample:

```sh
mkdir -p Games/MyGame/Scenes Games/MyGame/Source Games/MyGame/Config Games/MyGame/Content
```

Now create the project file, `Games/MyGame/MyGame.swproject`:

```text
shockwave_project 1
name "My Game"
game_module "my_game"
startup_scene "Scenes/main.swscene"
assets "Content"
```

That tells Shockwave the game's name, which C++ module holds its code (`my_game`, which
you'll write in a moment), which scene to open first, and where images and models go.
[Projects](Projects.md) lists every setting you can add.

## 3. Make a scene

A scene is a list of entities. An entity is a thing in the world, such as the floor, the
player or a light, and each one has components, such as a position or a mesh.

Create `Games/MyGame/Scenes/main.swscene`:

```text
shockwave_scene 2

entity "Environment"
  Environment
    sky_color 0.32 0.42 0.58 1
end

entity "Floor"
  Transform
    scale 20 1 20
  MeshRenderer
    mesh floor
    texture grid
end

entity "Player"
  Transform
    position 0 0.5 0
  MeshRenderer
    mesh cube
    tint 1 0.6 0.2 1
end

entity "Sun"
  Transform
    rotation -50 35 0
  Light
    type Directional
    intensity 2.5
end

entity "Camera"
  Transform
    position 0 6 10
    rotation -30 0 0
  Camera
    primary 1
end
```

You could also build this scene by clicking in the editor (the **Create** menu adds
cubes, lights and cameras). Text works just as well, and it's what AI tools write.
Every component and field is listed in [SceneSchema.md](SceneSchema.md).

## 4. Choose your controls

Games don't read keys directly. They read **actions** like "jump", and a bindings file
says which keys and buttons trigger each action. Players can then rebind them without
any code changes.

Create `Games/MyGame/Config/Input.swinput`:

```text
shockwave_input 1
bind move_left key A
bind move_right key D
bind move_forward key W
bind move_back key S
bind jump key Space

bind move_left pad_axis left_x -
bind move_right pad_axis left_x +
bind move_forward pad_axis left_y +
bind move_back pad_axis left_y -
bind jump pad south
```

`pad south` is the bottom face button: A on Xbox, Cross on PlayStation. You can also edit
bindings in the editor under **Window > Input**. [InputSchema.md](InputSchema.md) lists
every key and button name.

## 5. Write the game code

Create `Games/MyGame/Source/MyGame.cpp`:

```cpp
#include <Shockwave/Game.h>

using namespace shockwave;

// Lets the player move an entity around and jump.
struct Mover {
    float speed = 5.0f;       // units per second
    float jumpSpeed = 7.0f;   // how fast it leaves the ground
    float velocityY = 0.0f;   // runtime only: not saved
};
SW_COMPONENT(Mover, SW_FIELD(speed, 0, 20), SW_FIELD(jumpSpeed, 0, 20))

class MyGame final : public GameMode {
public:
    // Play has started: give the Player a Mover.
    void beginPlay(GameContext& game) override {
        if (EntityRef player = game.find("Player")) player.add(Mover{});
        SW_LOG(LogGame, Info, "My game started");
    }

    // Runs at a fixed rate (60 times a second by default).
    void fixedUpdate(GameContext& game, double dt) override {
        const float step = static_cast<float>(dt);
        Input& input = game.input();
        const float right = input.axis("move_left", "move_right");
        const float forward = input.axis("move_back", "move_forward");
        const bool jump = input.isActionDown("jump");

        game.world.each<Transform, Mover>([&](Transform& transform, Mover& mover) {
            transform.position.x += right * mover.speed * step;
            transform.position.z -= forward * mover.speed * step;

            const float ground = 0.5f;   // a 1-unit cube resting on the floor
            const bool onGround = transform.position.y <= ground;
            if (onGround && jump) mover.velocityY = mover.jumpSpeed;
            mover.velocityY -= 20.0f * step;   // gravity
            transform.position.y = std::max(ground, transform.position.y + mover.velocityY * step);
            if (transform.position.y <= ground) mover.velocityY = 0.0f;
        });
    }
};

SW_GAME("my_game", MyGame)
```

Here's what each part does:

- `struct Mover` is a **component**: plain data. `SW_COMPONENT` registers it with the
  engine. The fields listed with `SW_FIELD` are saved in scenes and editable in the
  editor, between the limits you give (0 to 20 here). `velocityY` isn't listed, so it's
  only used while the game runs.
- `MyGame` is the **GameMode**: the rules while the game is playing. `beginPlay` runs
  once when Play starts. `fixedUpdate` runs 60 times a second, no matter how fast the
  screen draws, so the game plays the same on every computer.
- `game.find("Player")` looks up an entity by its name in the scene.
- `game.input().axis("move_left", "move_right")` gives a number from -1 to 1 from two
  actions. With a stick it's analog; with keys it's -1, 0 or 1.
- `game.world.each<Transform, Mover>(...)` runs your code for every entity that has both
  components.
- `SW_GAME("my_game", MyGame)` connects it all. The name must match `game_module` in the
  project file.

Finally, tell CMake about the game. Create `Games/MyGame/CMakeLists.txt` with one line:

```cmake
shockwave_game_module(my_game)
```

That builds every `.cpp` file in the game's `Source/` folder. When you add more files
later, they're picked up automatically.

## 6. Build and play

Tell the build about your game (the sample stays in the list), then build:

```sh
cmake -B build "-DSHOCKWAVE_GAMES=Games/Sample;Games/MyGame"
cmake --build build -j
```

You only need the first command once. CMake remembers the list.

Open your project and play:

```sh
./build/bin/editor Games/MyGame
```

Press **Ctrl+P**, then click in the viewport so it has keyboard focus. Drive the cube
with **WASD** or the left stick, and jump with **Space** or the bottom face button.
Press **Ctrl+P** again to stop. Everything goes back to where it was before Play.

If Play is greyed out, the game module didn't build: the Console panel says why.

## 7. Change code while the game runs

Leave the editor open and playing. In `MyGame.cpp`, change the speed:

```cpp
    float speed = 10.0f;      // units per second
```

Save the file, go back to the editor and press **Ctrl+Alt+B** (Build > Build and reload
game code). The Console shows the build, then "Reloaded game code". Play restarts with
the new code, and the cube is twice as fast.

You can rebuild from anywhere, not just from the editor. An IDE, a terminal running
`cmake --build build --target game_my_game`, or an AI agent all work, because the editor
reloads whenever the module changes on disk. If the code doesn't compile, the old code
keeps running and the errors show in the Console.

## 8. Use your component in the editor

Your component is a real part of the engine now, just like the built-in ones:

1. Stop Play.
2. Choose **Create > Cube** to add a second cube.
3. In the Inspector, click **+ Component** and pick **Mover** (under "Gameplay").
4. Set its `speed` to 2.
5. Press **Ctrl+S** to save the scene, then **Ctrl+P** to play.

Both cubes now move together, the new one more slowly, because the code runs on every
entity with a Mover. Open `Scenes/main.swscene` in a text editor and you'll see the
cube saved with `Mover`, `speed 2` and `jump_speed`.

## 9. Check and run the game on its own

The player runs your game without the editor, the way your players will:

```sh
./build/bin/player Games/MyGame
```

The player can also check a project for mistakes, such as typos in scenes, missing
cameras or bad bindings. It works without a GPU, so it's handy in scripts and for AI
tools:

```sh
./build/bin/player --check --project Games/MyGame
```

It prints `0 problems in 1 scene` when everything is fine. Otherwise it lists each
problem with its file and line.

## Where to go next

- [The Shockwave Book](Book/README.md): a longer, gentle guide that builds a whole game
  and explains how every part of the engine works.
- [Projects](Projects.md): everything about game code, including GameInstance, systems
  that run on all cores, and every project setting.
- [Physics](Physics.md): real rigid bodies and character controllers instead of the
  hand-made jump above.
- [Assets](Assets.md): bring in your own images and 3D models.
- [Audio](Audio.md): sounds and music.
- [Plugins and hot reload](Plugins.md): what survives a reload, and swapping renderers.
