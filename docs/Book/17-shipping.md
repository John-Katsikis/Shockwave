# 17. Shipping your game

**In this chapter:** making the game smaller and faster, and turning the project into a
folder anyone can run.

## Make it smaller and faster

Two things happen to your assets automatically:

- **Texture compression.** Images are compressed to BC7, a format GPUs read directly.
  A texture takes about a quarter of the memory, and looks nearly the same. Compressed
  copies are cached in `Saved/Cache/Textures`, so it only happens once per image.
- **Levels of detail (LODs).** Each model mesh gets simpler versions with fewer
  triangles. Far away, where you can't tell the difference, the simpler one is drawn.

The project manifest controls them:

```text
texture_compression high   # high, small or off
mesh_lods 4                # how many simpler versions, 0 to 7
lod_error_pixels 1         # how far a simpler version may stray on screen, in pixels
```

A file called `<asset>.swimport` next to an asset overrides the settings for just that
asset. To see what the settings save:

```sh
./build/bin/player Games/OrbHunt --optimise --renderer vulkan --json optimise.json
```

or open **Window > Optimisation** in the editor. [Optimisation](../Optimisation.md) has
the details.

## Package it

Packaging makes a folder players can run, with no editor, no source code and no build
folder. First, make a Release build; Debug works but runs slowly:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

Then, in the editor, choose **Build > Package game** and press **Package**. Or from a
terminal:

```sh
./build/bin/player --package --project Games/OrbHunt/OrbHunt.swproject
```

It runs five steps, and stops at the first that fails:

1. **Build** the player, the renderer plugins and your game module, so the package is
   never out of date.
2. **Check** the project with `player --check`, then compress any textures not yet in
   the cache.
3. **Copy** the engine and your project into a new folder, strip what players don't
   need, and write the launcher and README.
4. **Verify** that the package stands on its own: every library it uses is inside it or
   on every Linux system, and the packaged game passes its own `--check` from inside
   the folder.
5. **Archive**: swap the new folder in for the old one and make the `.tar.gz`.

At the end it prints where everything went:

```text
package .../Games/OrbHunt/Saved/Packages/OrbHunt-1.0.0-linux-x86_64
archive .../Games/OrbHunt/Saved/Packages/OrbHunt-1.0.0-linux-x86_64.tar.gz
run     .../Games/OrbHunt/Saved/Packages/OrbHunt-1.0.0-linux-x86_64/OrbHunt
```

Inside the folder:

```text
OrbHunt-1.0.0-linux-x86_64/
  OrbHunt       run this to play
  README.txt    how to run it and what it needs
  Binaries/     the game program, engine libraries, shaders, fonts, your compiled game code
  Game/         your project: the .swproject, Content/, Scenes/, Config/
  Licenses/     the licenses of the engine and its libraries
```

Your `Source/` folder is left out; your code ships compiled. `Saved/` is left out too,
apart from the compressed textures, so players' machines never compress anything.

## Package settings

Settings live in `Config/Package.swpackage`. Every line is optional:

```text
shockwave_package 1
name "Orb Hunt"
version "1.0.0"
exclude "*.blend" "Content/Raw"
```

The Package game window edits this file for you. [Packaging](../Packaging.md) lists
every setting.

## Recap

- Textures compress to BC7 and meshes get LODs automatically; the manifest tunes both.
- Package from a Release build with Build > Package game or `player --package`.
- Packaging builds, checks, copies, verifies and archives, and stops at any problem.

You've now built a whole game and seen every part of the engine. The last chapter is a
map to keep handy.

Next: [Map of the code, and a glossary](18-code-map-and-glossary.md).
