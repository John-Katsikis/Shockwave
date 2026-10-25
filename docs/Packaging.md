# Packaging your game

Packaging turns your project into a folder you can hand to players. They run one file
and play, without the editor, the engine's source code or a build folder. Shockwave
also makes a `.tar.gz` of that folder, ready to upload or send.

Packages are made for the platform you package on: Linux or macOS. A macOS package ships
MoltenVK, so players need nothing installed, and runs on the macOS version it was built on
or newer. Windows packages will come with that port.

## Package your game

In the editor, choose Build > Package game, then press Package. The window shows each
step as it runs, and its output. When it's done, Open folder shows the package and Run
game starts it the way a player would.

From a terminal, run this in the engine folder:

```sh
build/bin/player --package --project Games/Sample/Sample.swproject
```

It prints each step, then where the package and archive are, and how to run it:

```
package /path/to/Games/Sample/Saved/Packages/ShockwaveSample-1.0.0-linux-x86_64
archive /path/to/Games/Sample/Saved/Packages/ShockwaveSample-1.0.0-linux-x86_64.tar.gz
run /path/to/Games/Sample/Saved/Packages/ShockwaveSample-1.0.0-linux-x86_64/ShockwaveSample
```

It exits with 1 when something stops it, and the lines starting with `error:` say why.

Package from a Release build. A Debug build works but runs slowly, and packaging warns
about it. To make a Release build:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

## What players get

```
ShockwaveSample-1.0.0-linux-x86_64/
  ShockwaveSample      run this to play
  README.txt           how to run it and what it needs
  Binaries/            the game program, the engine's libraries, shaders and fonts
  Game/                your project: the .swproject, Content/, Scenes/, Config/
  Licenses/            the licenses of the engine and the libraries in it
```

Players run `ShockwaveSample`, by double-clicking it or typing `./ShockwaveSample` in a
terminal. It's your project's player, renamed after your game, and it accepts the
player's options, like `--fps 144` or `--size 1920x1080`.

The packaged player always runs the project packaged with it, wherever it's started
from. `Binaries/shockwave-package.txt` tells it which one, and also records the
version, the platform, the build type and the oldest system the package runs on.

These parts of your project stay out of packages:

- `Source/`: your game code ships compiled, in `Binaries/modules`.
- `Saved/`: editor state, autosaves, screenshots and earlier packages. The one exception
  is `Saved/Cache/Textures`: your textures ship already compressed (see
  [Optimisation](Optimisation.md)), so players' machines never compress anything.
- `Tools/`, `CMakeLists.txt`, hidden files like `.gitignore`, and
  `Config/Package.swpackage`.
- Anything matching your own exclude patterns (see below).

## Change the package settings

Settings live in `Config/Package.swpackage` in your project. The Package game window
edits it, and so can you or an AI tool, as text. Every line is optional. A project
without the file packages with the defaults below.

```
shockwave_package 1
name "Space Ace"
version "1.2.0"
exclude "*.blend" "Content/Raw"
```

| Key | Default | What it does |
|---|---|---|
| `name` | the project's name | What players see. The program and folder are named after it. |
| `executable` | the name without spaces | The program's file name, like `SpaceAce`. |
| `version` | `1.0.0` | Goes into the folder name and the README. |
| `output` | `Saved/Packages` | Where packages go: relative to the project, or a full path. |
| `exclude` | nothing | File patterns to leave out. Repeat the key or list several. |
| `build` | `true` | Build the player and your game code first, so the package is never out of date. |
| `archive` | `true` | Also make a `.tar.gz` of the folder. |
| `strip` | `true` | Leave out symbol tables players don't need. Packages get about half as big. |
| `cpp_runtime` | `true` | Ship your machine's C++ runtime for older systems (see below). |

In the editor these are the fields of the Package game window. Save settings writes the
file without packaging, and Package saves it first. When the file changes on disk, the
open window picks up the change.

An exclude pattern without a `/` matches a file or folder name anywhere, like `*.psd`.
One with a `/` matches a path from the project folder, like `Content/Raw`. `*` matches
any run of characters within a name, and `?` matches one character.

The output folder can't be inside your project, except in `Saved/`, because the next
package would copy it. `player --check` reports that, and any other mistake in the
file, with the line it's on.

From the command line, these options change one run without editing the file:

| Option | Does |
|---|---|
| `--output folder` | Put the package in this folder |
| `--no-build` | Don't build first |
| `--no-archive` | Don't make the `.tar.gz` |
| `--dev-package` | A development package: allows Steam AppID 480 (see [Steam](#steam)) |

## What packaging checks

Each package goes through five steps. If one fails, packaging stops, and any earlier
package of the same version is left as it was.

1. **Build.** Builds the `player` target in the build folder the engine came from. That
   also builds the renderer plugins and your game module.
2. **Check.** Runs `player --check` on your project. A scene with a missing asset or
   another problem stops packaging here, with the problem listed.
   Then it compresses any textures not yet in the cache, as `player --optimise` does.
3. **Copy.** Copies the engine and your project into a new folder next to the old
   package, on a few threads of its own. Then it strips symbols and writes the launcher
   and README.
4. **Verify.** Makes sure the package stands on its own. Every library it uses must be
   inside it or on every Linux system, and nothing that never ships (`steam_appid.txt`,
   captures) is there. Then the packaged game runs its own
   `--check`, which proves it finds its project, code and plugins from inside the folder.
5. **Archive.** Swaps the new folder in for the old one and makes the `.tar.gz`.

Building and checking run without a window or sound device, so packaging also works on
a server, in CI or for an AI agent.

## Steam

A game whose `Config/Network.swnetwork` turns multiplayer on over Steam
([Networking.md](Networking.md#steam)) packages with these rules:

- It needs its own `steam_app_id`. AppID 480 (Spacewar, Valve's shared test app) stops
  packaging unless you ask for a development package: `player --package --dev-package`,
  or tick **Development package** in the editor's window. That choice is never saved in
  `Package.swpackage`, so a release can't ship as a test build by mistake.
- An engine built with the Steamworks SDK (`-DSHOCKWAVE_STEAMWORKS=ON`) ships Valve's
  runtime library (`libsteam_api.so`) in `Binaries/`; packaging fails if it's missing.
  The SDK itself never ships. The README says online play needs Steam running.
- An engine built without the SDK still packages the game, with a warning: it plays
  offline only, and its online buttons say why.
- `steam_appid.txt` and `.swnetcap` packet captures never ship, wherever they are in
  the project; Verify fails if one ever reaches the package.
- A packaged Steam game started outside Steam asks Steam to start it (except a
  development package, which runs as it was started; put a `steam_appid.txt` with 480
  next to the program to test it outside Steam).

## Which Linux systems can run it

Linux programs need a C library (glibc) at least as new as the one they were built
against. Packaging finds the oldest version your package needs. It shows it in the log,
writes it to the README and records it in `Binaries/shockwave-package.txt`.

The C++ runtime (libstdc++) works the same way, and rolling distributions like CachyOS
and Arch ship a newer one than LTS distributions like Ubuntu. So by default the package
also carries your machine's C++ runtime in `Binaries/cpp-runtime`. The launcher uses it
only when the player's own is too old for the game, so a newer system keeps its own.

For the widest reach, package on the oldest distribution you want to support, for
example in a container. Players also need a Vulkan 1.3 driver: Mesa 23 or newer for AMD
and Intel, or NVIDIA's driver 525 or newer.

## Third-party licenses

The engine is MIT licensed, and the libraries in it each have a license that asks you
to include it when you ship. Packages do that for you, in `Licenses/`. They come from
`build/bin/licenses`. If you add a library to the engine, copy its license there in its
CMake file, the way `cmake/RmlUi.cmake` and `Source/Runtime/Packaging/CMakeLists.txt`
do.

## Limits

- Packages are for the platform you package on, which is Linux for now.
- There's no icon or desktop menu entry yet.
- Packages use the modular build: the engine as shared libraries next to the program.
  A build with `SHOCKWAVE_MODULAR=OFF` packages too, as one program.

## How it works

`Packaging/Package.h` (`Source/Runtime/Packaging`) holds the settings file and
`packageProject()`. The editor's window and `player --package` both call it. The editor
runs it on a background thread, so editing stays smooth while it works. The copy step
uses a few threads of its own and leaves the engine's job workers free for frames.

The new package is built in a `.partial` folder and only replaces the old one after it
passes Verify. The Verify step reads each program and library in the package to see
which libraries it needs and where it looks for them. It has to find every one of them
inside the package, through `$ORIGIN`, or on every Linux system.
