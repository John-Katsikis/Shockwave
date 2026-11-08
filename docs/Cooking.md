# Cooking

A **cook** works out everything a release of your game is made of: every level in the
[catalog](Levels.md), everything each one reaches, and the files the game reads that no
level names. It writes that down in a manifest, so a release can be checked against it.

```sh
player --project Games/ShippingDemo --cook               # Saved/Cook/linux-x86_64/Cook.swcook
player --project Games/ShippingDemo --cook --release     # held to the release policy
player --project Games/ShippingDemo --check --release    # the policy, without writing
player --cook-info Games/ShippingDemo/Saved/Cook/linux-x86_64/Cook.swcook
player --cook-info Cook.swcook --verify Games/ShippingDemo   # every file still matches
player --project Games/ShippingDemo --package --release  # a package of only what the cook lists
```

`--json report.json` writes what the cook found (with `--cook-info`, the manifest as
JSON), and `--output file` writes the manifest somewhere else. A failed cook leaves the
last good manifest where it was. The cook reads the project; it doesn't change it.

## The manifest

`Cook.swcook` is plain text, one record a line, every list sorted:

```
swcook 1
content_id ae10ffecdb8cec30
build_id 523c15fdff0e0510
target linux-x86_64
build_config "Release"
engine_version "0.1.0"
plugin_abi 22
release true
license "SDL3-LICENSE.txt"
application "Reactor Run"
game_module "shippingdemo"
entry "menu"
level factory "Scenes/factory.swscene" b07704161e945302
root "Content/Localization/fr.swstrings" "localization catalog"
asset "Content/UI/demo.rcss" "stylesheet" 2913 a967d0ddf2bab32e 89a979254fa9bcfb ending factory training
```

- **level**: the catalog's id, its scene and the scene's closure hash, which changes
  when anything the level reaches changes.
- **asset**: its path in the project, kind, size, hash, closure hash and the levels that
  reach it (none: only a root needs it). Shapes and textures made in code show as
  `builtin:cube`.
- **root**: a file the game needs that no level names, and why.
- **content_id** is a hash of the game's records: the same content gives the same ID on
  any machine, from any folder. **build_id** adds the engine, its configuration, the
  target, the plugin ABI and the licenses.

Nothing in it depends on when or where it was made: no times, no absolute paths. Two
cooks of the same content are the same file byte for byte, so a diff between two
manifests says what changed. The programs themselves aren't claimed to be
byte-for-byte reproducible, so their bytes stay out of both IDs.

`--cook-info` refuses a manifest whose IDs don't match its records: it was edited or
damaged after the cook. `--verify folder` checks every listed file under that folder (a
project, or a package's `Game` folder) against its size and hash.

## Files no level names

A level's closure is what its scene refers to, all the way down. The game also reads:

| Root | Why |
|---|---|
| the `.swproject` and `Config/*` settings | read at start |
| the game circuit | runs for the whole game |
| `Content/Localization/*.swstrings` | the game's text |
| fonts (`.ttf`, `.otf`) anywhere in `Content` | any document may use one |
| `Content/Shell/*.rml` | the shell's own screens |
| `Content/Shell/credits.txt` | the Credits page's text |
| what `include` patterns name | what game code loads by name |

Game code that loads a file by name, like `audio.playClip("Audio/shot.wav")`, can't be
followed: list those files in `Config/Package.swpackage`:

```
include "Content/Audio/*.wav"
```

`*` is any run of characters within a name, `**` any run at all, `?` one character. An
include pattern that matches nothing is a warning.

## What a release may not do

`--release` (on `--cook` or `--check`) fails, with every problem, when:

- the engine isn't an optimised build (Release, RelWithDebInfo or MinSizeRel): a Debug
  build, or one with no configuration, runs many times slower;
- something a level or root needs is missing, has the wrong case, lies outside the
  project, or can't be read ([AssetResidency](AssetResidency.md#problems));
- a name only code registers can't be found (`AssetUnknownName`, a warning otherwise);
- an `exclude` pattern leaves out something a level needs;
- a level uses a `.blend` file, which needs Blender to load: export it to `.glb`;
- a scene, material, document, catalog or setting names an absolute path from a
  developer's machine (their home folder, a drive letter, the project's own folder);
- the game plays over Steam with AppID 480, Valve's shared test app;
- the engine's licenses aren't there to ship.

A development cook reports the same problems except the release-only ones, which it
lets through.

## Release packages

`player --package --release` (or **Release** in the editor's Package window) makes a
package from the cook:

1. After the usual build and checks, it cooks with the release policy (as
   `--cook --release`), so a release package needs an optimised engine and passes every
   rule above. It can't also be a `--dev-package`.
2. `Game/` gets only what the cook lists, plus the cook itself as `Game/Cook.swcook` and
   the compressed textures made from those files. Docs, tools, notes, test files and
   anything else no level or root reaches stay behind. The packager says how many files it
   left out.
3. Before the package replaces the last one, every file in `Game/` is checked against
   the cook, byte for byte, and anything the cook doesn't list fails the package.
4. `Binaries/shockwave-package.txt` records `release true` and the cook's `content_id`
   and `build_id`, so the folder and its archive say which content they hold.

A failed release (a cook problem, a missing file, a cancel) leaves the last good package
as it was. Anyone can check a release later with its own player:

```sh
ReactorRun-1.0.0-linux-x86_64/Binaries/ReactorRun --cook-info Game/Cook.swcook --verify Game
```

A development package (`--package` without `--release`) still ships the whole project
folder, less what [Packaging](Packaging.md#change-the-package-settings) leaves out.

## In the editor

**Build > Package game** has a **Release** checkbox, never saved with the settings, like
**Development package**, and the two can't both be on. It also shows:

- **Target**: the platform this engine packages and cooks for.
- **Cook**: the project's last cook for that target. It shows whether it's a release cook,
  how many levels, files and roots it has, its size, and its content and build IDs. Under
  that is a table with each level's files, how many of those it shares with other levels,
  and its size. **Verify files** checks the project's files against the cook and lists any
  that changed since, which means it's time to package again. The section rereads the
  manifest whenever a package or `player --cook` writes it.
- After a release package: how many files came from the cook, every one checked, and how
  many project files were left out.

`editor --package-release` presses the window's Package button with Release ticked, prints
the result and exits, which is how the `editor_package_release` test drives it.

## Targets

`--target` picks the platform; the default is the one the engine was built for. A cook
records the engine and its modules, so it can only be for that platform: another target
fails and says to cook on a machine of that kind.

## Tests

`cook_reproducibility` (two cooks, and one of a copy in a folder with spaces and
non-ASCII letters from another working folder, give the same manifest; `--verify` and
`--json`) and `cook_dependencies` (a stylesheet three levels share changes exactly those
three levels and the content ID; a file changed after the cook, an edited manifest, a
missing texture, an excluded document, a code-played sound without its include pattern,
an absolute path in release, `--check --release`, and the wrong target).

`package_release` packages a clean copy, with no `Saved/` folder, in a folder with spaces
and non-ASCII letters, from another working folder. It checks that only cooked files ship,
that the marker's build ID is the cook's, and that the packaged player's `--verify` passes.
Then it moves the package and makes it read-only, and plays menu, training and factory,
saves, quits and resumes from it. It also checks the failures:

- a missing game module tells players to install again, not to build;
- a missing engine library is named by the system;
- an absolute path, or a missing texture, fails the release and keeps the last good
  package with nothing half-made;
- `--release --dev-package` is refused.

The `package` unit test checks that a cancelled package keeps the last one, that an engine
folder without a player says so, and the cook report the editor shows.
`editor_package_release` makes a release package from the window.
