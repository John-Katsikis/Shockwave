# Where a game writes: per-user folders

An installed game never writes beside itself. Its install folder may be read-only (Program
Files, a Steam library, a signed app bundle), and several people may share one machine.
Everything a game writes goes into a per-user folder instead:

- settings;
- save games;
- logs;
- caches (the pipeline cache);
- crash reports.

This is S01 of [the shipping plan](ShippingImplementation.md). The code is
`Platform/UserPaths.h`.

## Where the folders are

The folders are named by the game's **application ID**. That's the release contract's
`application_id` ([Release.md](Release.md)). A project without a contract uses
`shockwave.<game module>`, lowercase. The ID is never taken from the display name, so
renaming the game doesn't lose anyone's saves.

| How it runs | Root |
| --- | --- |
| Installed, Linux | `$XDG_DATA_HOME/<id>/`, or `~/.local/share/<id>/` when `XDG_DATA_HOME` isn't set or isn't absolute |
| Installed, Windows | `%APPDATA%\<id>\` (Unicode user names work) |
| Installed, macOS | `~/Library/Application Support/<id>/` |
| `--user-dir <folder>` | that folder, with the installed layout |
| From source (player, editor Play) | `<project>/Saved/`, the layout it always had |

A game counts as installed when it runs from a package (`player --package`).

The installed layout:

```text
<root>/
  profiles/<profile>/
    Config/     Graphics.swgraphics, imgui.ini
    Saves/      <slot>.swsave
  Logs/
  Cache/        pipeline-cache/
  Crashes/
```

Run from source, the project's `Saved/` keeps its old layout, so nothing changes during
development:

- `Saved/SaveGames/` holds the default profile's saves;
- `Saved/Profiles/<profile>/SaveGames/` holds another profile's;
- `Saved/Config/`, `Saved/Logs/`, `Saved/Cache/` and `Saved/Crashes/` hold the rest.

Folders are created only when something is written to them. Resolving the paths creates
nothing.

## Profiles

A profile keeps one player's settings and saves apart from another's. The local player is
`default`. A profile ID is 1 to 64 lowercase letters, digits, `-` and `_`, starting with a
letter or digit (`steam-76561198000000000`, for example). Profiles share the logs, cache and
crash folders, but never each other's config or saves.

## What it refuses

- **No per-user folder.** If `HOME` isn't set (or `APPDATA` on Windows), an installed game
  refuses to play or save, and says why. It never falls back to the current directory.
- **A folder inside the game.** `--user-dir` inside the install is refused, and so is an
  OS folder that resolves inside it.
- **A bad ID.** An application or profile ID outside the rules above is refused. A bad
  `--profile` is a usage error (exit 2).
- **A folder that can't be made.** A permission or disk error names the folder. It's never
  swallowed.

## The command line

```sh
player --user-paths [--json -|file]        # where this run reads and writes
player --profile p2 ...                    # another player's settings and saves
player --user-dir <folder> ...             # somewhere else entirely (tests, CI, portable installs)
player --import-legacy-saves <folder> [--preview]
```

`--user-paths` prints the source (`installed`, `override` or `development`), the application
ID, the profile and each folder, and says whether each one exists yet. It exits 1 when the
folders can't be resolved. The JSON is `format 1`, `mode "user-paths"`.

`--import-legacy-saves` copies save games from an older place into the profile. That might
be a project's `Saved/SaveGames`, or a folder a previous version used. For each slot:

- it reads the slot's header first, and skips anything that isn't a save game;
- it copies the slot and never moves it, so the original stays;
- it never writes over a slot the profile already has, because that one may be newer.

It ends with `copied N, kept N, failed N` and exits 1 if anything failed. `--preview` prints
the same report and changes nothing.

Tests and CI always pass `--user-dir`, so they never touch real player data.

## In the editor

**Project > Release readiness** has a **Player data** section:

- where the installed game will write, for the application ID as currently typed;
- where Play in the editor writes;
- an **Open** button for each folder that exists;
- **Import development saves**, which copies `Saved/SaveGames` into the installed default
  profile, with the same rules as the command line.

## The release gate

`user_paths` passes when the installed folders resolve for the contract's `application_id`
on this machine, outside the game's own folder. It fails with the reason otherwise
(`player --release-info`).

## Turning it off

The old places are still there, behind the same switches:

- `--no-user-graphics` ignores the profile's graphics settings;
- `--graphics <file>` uses a graphics file of your choosing;
- `--no-pipeline-cache` keeps no cache.

Without `--user-dir`, a run from source writes to the project's `Saved/`, as before.

## Moving from before S01

Before S01, an installed game put its graphics settings, pipeline cache and window layout
in SDL's preferences folder, `~/.local/share/Shockwave/<game>/` on Linux. Those now live in
the profile, so a player's graphics settings start from the game's defaults once.

Saves used to go inside the package, in `Game/Saved/SaveGames/`. The new version doesn't
read them there, and it doesn't move them on its own. Bring them over with:

```sh
<package>/Binaries/<game> --import-legacy-saves <old package>/Game/Saved/SaveGames
```
