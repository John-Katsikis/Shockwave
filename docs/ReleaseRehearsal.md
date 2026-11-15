# Release rehearsal

Automated tests prove the package installs and plays. A rehearsal proves that a person
can do it with nothing but the package. Before a release, someone who has never seen the
game installs and plays it on a machine that has never seen the engine. You watch, and
write down every time you had to help.

It's the last gate of [the shipping plan](ShippingImplementation.md#23-s15--documentation-and-release-rehearsal)
(S15). `player --release-info` and **Project > Release readiness** show it as
`release_rehearsal`.

## What you need

- **A new tester.** Someone who hasn't played the game or watched it being made. A
  tester who rehearsed an earlier build has learned it. Use a new one after a failed
  rehearsal.
- **A clean machine and profile.** No engine checkout, no build folders, no SDKs, no
  copies of the game's saves or settings. A new user account on a spare machine works.
  So does a fresh virtual machine, as long as it has a real GPU for the platform you're
  checking.
- **The package**, exactly as players will get it: the release package (or its archive)
  from `player --package --release`, for this version and build.
- **The player instructions**: the few lines a player would get from the store page or a
  readme. Write them before the rehearsal. They're part of what's being tested.

Nothing else. No verbal walkthrough, no "it's in that folder", no shared screen.

## Player instructions

Keep them as short as a store page would. For example:

```text
Two Rooms 1.0.0 for Linux (x86-64)

1. Unpack TwoRooms-1.0.0-linux-x86_64.tar.gz anywhere.
2. Run TwoRooms in the folder it makes.

Controls: WASD or the left stick to move, Escape or Start to pause.
Your saves and settings are kept for you between runs.
```

If the tester needs something the instructions don't say, that's an intervention. Fix the
instructions or the game, not the tester.

## During the rehearsal

Ask the tester to:

1. Install the game the way the instructions say.
2. Start it and get to the main menu.
3. Change a graphics setting, a volume and one control binding, then quit and start it
   again. The changes should still be there.
4. Start a new game and play to the first checkpoint, then quit from the pause screen.
5. Start the game again and **Continue**. It should resume at the checkpoint.
6. Play to the end and back to the menu.
7. If the game is online: host or join through the real service, play a match through a
   map change, and leave.

Say nothing unless the tester is stuck. Write down where they hesitated, what they said
and what they tried.

### What counts as an intervention

Anything a developer did that a player couldn't. Each one fails the rehearsal:

- setting a path or an environment variable;
- copying in a missing library, driver or runtime;
- editing, moving or deleting a save or settings file;
- opening a terminal, a debug console or a developer menu;
- explaining something the game or the instructions should have made clear;
- restarting the machine or the game to get past a hang.

A question the tester answers by trying something in the game isn't an intervention.
Neither is a note about something they found awkward. Record that as a note.

## Record it

In **Project > Release readiness**, open **Release rehearsals > Record a rehearsal**. Fill
in the tester (a label like "Tester B" is enough), the machine, the platform, the package
and one line per intervention, then press **Save record**. The record goes to
`Release/Rehearsals/<date>-<tester>-<platform>.swrehearsal`, and records are never
overwritten.

The file can be written by hand too:

```text
shockwave_rehearsal 1
tester "Tester B"
recorded "2026-11-20T15:00:00Z"
machine "Borrowed laptop, new user account"
platform linux-x86_64
package "TwoRooms-1.0.0-linux-x86_64"
version "1.0.0"
build_id "eb9dfd1a69453e97"
intervention "had to install libvulkan1 before the game would start"
note "found Continue without help"
result failed
```

Every key but `intervention` and `note` is required. `build_id` is the package cook's
build ID (`player --cook-info` on its cook manifest). It may be `""` when you don't
know it. Reading is strict: an unknown key, a repeated key or a missing one makes the file
unreadable, with its line. A record that says `result passed` with an intervention in it
doesn't read either.

## The gate

`release_rehearsal` looks at the newest record (by `recorded`) for the contract's
`release_version`, on a platform the contract names, of the current build when the record
names one:

| Newest record | Gate |
| --- | --- |
| passed, no intervention | `passed` |
| failed, or any intervention | `failed`, naming the first intervention |
| none for this version, platform or build | `unverified` |

A failed rehearsal stays failed until a newer one passes. Fix what needed the help, make
a new package, and rehearse again with a tester who hasn't seen the game.

## Code

`Projects/Rehearsal.h`: the record, its file format, and `rehearsalGate`. The test is
`release_rehearsal` (`Tests/RehearsalTest.cpp`).
