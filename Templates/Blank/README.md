# Your Shockwave project

This folder owns your game. Keep gameplay C++ in `Source/`, scenes in `Scenes/`,
assets and circuits in `Content/`, and settings in `Config/`. `Saved/` contains local
editor state and generated files. Engine code stays in the Shockwave checkout.

The project starts with a floor, cube, sun and camera, plus an empty game mode.
It has no dependency on the Sample or Arena games.

Before the first Play, register this folder in the engine build. From your Shockwave
checkout, replace the example game path below with this folder's absolute path:

```sh
cmake -S . -B build "-DSHOCKWAVE_GAMES=/absolute/path/to/YourGame"
cmake --build build --target player editor -j4
./build/bin/player --project /absolute/path/to/YourGame --check
./build/bin/editor /absolute/path/to/YourGame
```

`SHOCKWAVE_GAMES` is a semicolon-separated list. Keep the folders for other games you
want built in that list. This changes build configuration, not engine source files.
Building player/editor also builds the registered game modules. Subsequent C++ edits
can use **Build > Build and reload game code** in a modular editor. With static
modules (including the default Windows build), rebuild and relaunch player/editor.

Run `player --project <this-folder> --schema Saved/SceneSchema.md` for this game's
component reference and `--node-schema Saved/CircuitNodes.md` for its circuit nodes.
Use `--check` after changing scenes, assets, settings or circuits. The engine's
`docs/StartingProjects.md` explains the complete workflow.
