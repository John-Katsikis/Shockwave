# Diagnostics

When a game misbehaves on a player's machine, Shockwave keeps what's needed to understand
it later: a log file, a crash report with the moments before the crash, and a note of
whether the last run ended cleanly. All of it stays on that machine, in the player's own
folders ([UserPaths.md](UserPaths.md)). Nothing is ever uploaded.

This is S12 of the shipping plan: part 1 keeps the evidence (logs, crash reports, symbols);
part 2 recovers (the menu's recovery page, a lost GPU device, child processes without a
shell, paths from data files checked, and the release gate).

## What a run keeps

| What | Where | Who |
|---|---|---|
| Log file | `Logs/player.log`, previous runs as `player.1.log` .. `player.5.log` | a player playing |
| Session marker | `Logs/player.session`, removed at a clean exit | a player playing |
| Crash report | `Crashes/player-<UTC time>-<pid>.crash.txt` (Windows adds a `.dmp`) | every run |
| Recent-log ring | memory: the last 64 KiB of log lines | every run |

Run from source, these folders are the project's `Saved/Logs` and `Saved/Crashes`; an
installed game uses its per-user folder; `--user-dir` picks one for a run (tests always
do). The Crashes folder is only made when a crash happens.

"A player playing" means the player started to play: not a tool (`--check`, `--cook`,
`--net-test`), a measurement (`--benchmark`, `--perf-run`), a screenshot or an automated
flow test. Tools still get crash reports.

A log file stops, with a note, at 16 MiB or when the disk is full; the ring goes on, so a
crash report still has the last lines.

### The session context

The engine keeps a few facts current, which go into crash reports and the session
marker:

| Key | Set by |
|---|---|
| `program`, `engine`, `platform`, `installed` | the player at start |
| `project`, `application_id`, `build_id` | the player, from the project and its cook |
| `renderer`, `gpu`, `gpu_driver`, `vulkan` | the engine and the Vulkan context |
| `first_frame` | the engine, once the first frame is presented |
| `renderer_state` | the renderer: `device lost` once the GPU device is lost |
| `graphics`, `safe_mode` | the player (the preset, or `custom`) |
| `level`, `level_epoch`, `travel_phase`, `travel_error` | level travel |
| `last_save` | the save system (slot and revision) |
| `last_error` | the last error logged |

Game code can add its own with `shockwave::diagnostics::setContext("key", "value")`
(Core/Diagnostics.h); an empty value removes a key. There are at most 48 keys, 256 bytes
each.

### What is never kept

The user's home folder is written as `~` in log files, the ring, the context and crash
reports, and any run of 96 or more hex digits (tickets, tokens, keys) as `[redacted]`.
Authentication tickets, access tokens and voice are never logged in the first place;
`diagnostics_cli` checks an authentication failure's log for them. Standard error, which
only a developer sees, is unchanged.

## Crash reports

A fatal fault (a segfault, an abort, an illegal instruction, a bus error, a division
trap) writes a report and then lets the process end the way it would have without one,
so a debugger, a core dump or CTest still sees the original signal. One line goes to
standard error: `player crashed (SIGSEGV). A report was written to ...`.

```text
shockwave_crash 1
program player
signal SIGSEGV (11)
fault_address 0x0
time_unix 1794438297
[context]
level: factory
...
[frames]
0x55849140d963
...
[modules]          Linux: the process's memory map, to find each frame's binary
[symbols]          macOS: the system's names for each frame
[log]
LogGame: Loading the factory
...
[end]
```

The handler is written to be safe where it runs: on Linux and macOS it runs on its own
stack and uses only calls that are safe in a signal handler (`open`, `write`, `mkdir`,
`read`, `close`, `sigaction`, `raise`) on memory set up beforehand, plus `backtrace()`,
whose library is loaded at start. It never allocates, takes the log's lock, formats with
the C++ library or touches Vulkan. On Windows an unhandled-exception filter writes the
report and a minidump with the system's `MiniDumpWriteDump`, and `abort()` is caught too.

`--no-crash-handler` turns crash capture off for a run (the editor has the same flag).

### Naming the frames

```sh
build/bin/player --symbolize Saved/Crashes/player-20261111-230457-3822.crash.txt
```

```text
player crashed: SIGSEGV (11)
  #0  shockwaveTestCrashHere(char const*)  (crash_program +0x4963)
  #1  shockwaveTestCrashOuter(char const*)  (crash_program +0x497f)
  #2  main  (crash_program +0x3dea)
  ...
context:
  level: factory
last log lines:
  LogGame: Loading the factory
```

It reads each frame's binary from the report's memory map and names the function from
the binary's symbol table (the crash handler's own frames are left out). `--json file`
writes the same as JSON. Without symbols a frame shows `??` and its offset.

A package strips symbols to halve its size. Before it does, it keeps each program and
library's full copy beside the package, never in it or its archive:

```text
ShockwaveSample-1.0.0-linux-x86_64/            what players get
ShockwaveSample-1.0.0-linux-x86_64-symbols/    keep this with the build
  shockwave-symbols.txt
  1e9e197d63f910b5c7185238b63c97d07218c25f/ShockwaveSample
  ...
```

Each copy is filed under its GNU build ID, which the stripped binary still carries, so a
player's report always meets the symbols of the exact build that crashed:

```sh
build/bin/player --symbolize report.crash.txt --symbols Saved/Packages/ShockwaveSample-1.0.0-linux-x86_64-symbols
```

`keep_symbols false` in `Config/Package.swpackage` (the editor's Package window: Keep
symbols) turns it off; with `strip false` nothing is stripped, so nothing needs keeping.
Function names come from the symbol table; line numbers need a build with debug
information (`-DCMAKE_BUILD_TYPE=RelWithDebInfo`) and `addr2line` on the kept copy.

On Windows open the `.dmp` in Visual Studio or WinDbg with the build's PDB files; macOS
reports carry the system's names for each frame. Naming Windows and macOS frames with
`--symbolize` isn't done yet.

## Did the last run end cleanly?

The player writes `Logs/player.session` when it starts playing (the time, its process,
and the context, rewritten as levels change) and removes it when it exits, including
after an error. If it's still there at the next start, the last run crashed or was
killed. The player then logs:

```text
LogEngine: warning: The last run (started 2026-11-11T23:04:57Z, level factory) didn't end cleanly;
its report is ~/.local/share/shockwave.arena/Crashes/player-....crash.txt. Start with --safe-mode for safe graphics
```

A marker whose process is still running belongs to another copy of the game: it's left
alone and this run writes its own. Nothing is deleted and no setting is reset
automatically.

### What the player is offered

Two places, depending on how far the last run got:

- **Before any window opens**, when the last run never showed a frame (no `first_frame`) or
  lost its GPU device: a system dialog asks *Start with safe graphics* or *Start normally*.
  A crash in startup or pipeline warmup would happen again before any menu could help.
  Without a display (tests, CI, a server) nothing is asked: the log says
  `After the last run: no dialog shown`.
- **On the main menu** (AppShell, docs/AppShell.md), otherwise: the menu opens on its
  recovery page, saying when the last run started, the level it was in and its crash
  report's name, with
  - **Safe graphics**: the game's lowest tier for this session, applied now. The player's
    graphics file isn't written (it changes only if they apply settings themselves);
    `--safe-mode` also starts windowed and without the pipeline cache.
  - **Restore last save**: the newest save this game and profile can load, as Continue picks
    it (S06 saves are written whole, so a half-written one is never "last").
  - **Open logs**: the folder of log files.
  - **Main menu**: puts the page aside.

`--no-recovery-prompt` turns both off. Commands for scenarios (`--flow-test`):
`safe_graphics`, `restore_save`, `open_logs`, `dismiss_recovery`; values `flow.recovery_text`,
`flow.recovery_report`, `flow.can_safe_graphics`, `flow.safe_graphics_on`,
`flow.can_open_logs`. A flow test keeps no marker of its own but reads the last one, so a
scenario can test the page.

## A lost GPU device

When Vulkan reports `VK_ERROR_DEVICE_LOST` (a driver reset, a hang, the GPU removed), the
device can't be used again in this process:

1. The renderer stops: nothing more is recorded or submitted, `beginFrame` returns false,
   and `renderer_state` becomes `device lost` (`Renderer::deviceLost()`).
2. The engine calls `Application::onRendererLost(reason)` once. The default logs and quits.
3. The player stops the game there: nothing more is simulated or saved, so the last save is
   still the last good one. It asks (a system dialog, when there's a display):
   *Restart with safe graphics*, *Restart* or *Quit*. A restart runs the same program with
   the same arguments (plus `--safe-mode`) after ending this run's session; Quit leaves the
   session marker, so the next start offers safe graphics. Without a display it quits with
   exit code 1 and says so.
4. The editor writes unsaved changes to the scene's autosave (offered back when the scene is
   next opened), says so, and closes.

Real device loss can't be caused safely on demand, so the path is tested by injection:
`player --inject-device-lost N` behaves from frame N as if the device were lost, and every
message says it was injected. A packaged game refuses the flag. Producing a real loss
(a TDR on Windows, a GPU reset on Linux) stays a manual check.

## Child processes

The packager (the player's checks, cook and texture compression, `cmake --build`, `strip`,
`tar`), the editor's game-code build and its Run game start programs with
`shockwave::runProcess` (Core/Process.h): a program and an argument vector, an explicit
environment, a working folder, stdout and stderr captured line by line, a timeout and a
cancel. No shell ever sees an argument, so a project in `My "Game"; $(rm -rf ~) & 100%/` is
only a folder name. On Windows the command line is encoded the way the C runtime decodes
it.

A timeout or cancel ends the child and what it started: its process group (SIGTERM, then
SIGKILL after two seconds) on POSIX, its job object on Windows. A Linux child also goes if
the thread waiting for it dies. Cancelling a package in the editor stops the tool that's
running. `startDetachedProcess` starts something that outlives the caller (Run game, a
restart); `runShellCommand` is the separate, explicit call for a developer command that
needs a shell, and nothing in packaging or the game uses it.

## Paths from data files

- A cook manifest is at most 64 MiB, and each file it names must be a relative path that
  stays in the game folder on every platform: no `..`, no absolute path or drive, no `\`
  or `:`, no names Windows reserves (`CON`, `NUL`, `COM1`...) or can't make (a trailing
  `.` or space). No two may differ only by case.
- A package can't hold two files that differ only by case.
- A link in the project to a file inside it ships as that file; one that leads out of the
  project (or nowhere) stops packaging. Links to folders aren't followed.
- The packager makes its staging folders (`<package>.partial`, `<package>-symbols.partial`)
  itself and marks them; only a marked folder is ever deleted. An unmarked one with that
  name stops packaging and stays as it is. (The package and symbols folders it replaces
  were already checked for their own markers.)
- Saves (S06) and network messages (S11) check sizes before they allocate.

## Release readiness

The `crash_recovery` gate (`player --release-info`, Project > Release readiness) fails while
the game's own crash reports are unresolved: the project's `Saved/Crashes` (from source) and
the installed game's `Crashes` folder on this machine. Fix the crash, then **Mark resolved**
in the window's **Fatal errors** section, which moves the report (and its `.dmp`) into
`Crashes/Resolved/`. It's also failed when packages strip without keeping their symbols,
unverified where crash capture isn't supported, and passed otherwise. A last run that ended
uncleanly without a report (killed, or a lost GPU device) is noted, not failed.

## player --diagnostics

```sh
build/bin/player --project Games/Arena/Arena.swproject --diagnostics [--json file]
```

```text
program player
engine 0.1.0 Release (linux-x86_64)
project Shockwave Arena
application_id shockwave.arena
build_id none (not cooked)
logs ~/.../Saved/Logs
crashes ~/.../Saved/Crashes
crash capture on
log files 2
  player.log (2210 bytes)
  player.1.log (1974 bytes)
previous run unclean (started 2026-11-11T23:04:57Z, level factory)
previous run's report ~/.../Saved/Crashes/player-20261111-230457-3822.crash.txt
crash reports 1
  player-20261111-230457-3822.crash.txt SIGSEGV (11)
advice: start with --safe-mode; player --symbolize <report> names the frames
```

It only reads: the marker stays for the next real start.

## Safe mode

```sh
build/bin/player --safe-mode
```

Plays with the game's lowest graphics tier (`Config/ReleaseTier.swtier`'s fallback, else
the Low preset) at native resolution, in a window, without the pipeline cache or its
warmup. The player's own graphics file is neither read nor written, so their settings
are still there afterwards; options given on the command line still apply on top. Part 2
offers it from the game itself after a crash.

## Command line

| Flag | Does |
|---|---|
| `--diagnostics [--json file]` | what's kept and whether the last run ended cleanly |
| `--symbolize report [--symbols folder]... [--json file]` | name a report's frames |
| `--safe-mode` | lowest graphics, windowed, no pipeline cache |
| `--log-file path` | this run's log there, for any run (tools too), without rotating |
| `--no-log-file` | no log file |
| `--no-crash-handler` | no crash reports |
| `--no-recovery-prompt` | no safe-graphics dialog or recovery page after an unclean exit |
| `--inject-device-lost N` | developer test: the GPU device is lost at frame N (refused in a package) |

## The editor

The editor keeps the same for itself beside its preferences (`~/.config/shockwave/Logs/editor.log`,
`Crashes/`, `editor.session`). Window > Diagnostics shows whether its last session and the
project's last player run ended cleanly, its log file, and the crash reports of both,
each named frame by frame with its context and last log lines, ready to copy.

## Tests

| Test | Proves |
|---|---|
| `diagnostics` | the ring keeps the last 64 KiB; context limits, newlines and `last_error`; redaction of the home folder and long hex runs; log files rotate (keeping N), stop at their size limit and on a full disk (`/dev/full`) while the ring goes on; the session marker: clean exit, unclean exit with its context and report, another live copy; reports newest first; the crash folder is made only on a crash; frames named from this process's own memory map |
| `diagnostics_cli` | `crash_program` crashing by segfault, abort and trap writes reports with context, frames, memory map and the last log lines (the fake ticket redacted); `--symbolize` names `shockwaveTestCrashHere`, `shockwaveTestCrashOuter` and `main`, and from kept symbols after the binary is stripped (not without them); `--diagnostics` (text and JSON) sees the unclean exit and its report without removing the marker; a game run warns, keeps a log, rotates it and removes its marker; no Crashes folder without a crash; `--no-log-file`; tools keep no log; `--safe-mode`; an authentication failure's log has no ticket |
| `package_cli` | a Linux package keeps its symbols beside it, by build ID, outside the package |
| `process` | arguments full of shell syntax, quotes, backslashes, `%`, newlines and Unicode reach the child byte for byte and never run; the environment (set, removed, not inherited), a working folder with such a name, exit codes, stdout and stderr lines; a missing program; a timeout that ends the child and its grandchild; a cancel; a detached start; Windows command-line encoding |
| `failure_recovery` | an injected lost device stops the game (exit 1) and leaves the marker; `--diagnostics` and the next run see it; the menu's recovery page: Safe graphics (the graphics file not written), Restore last save; `--no-recovery-prompt`; after a real crash the page names its report |
| `package` | cook paths that escape, are absolute or differ only by case are refused; an unmarked staging folder stops packaging and is kept, a marked one is taken over |

`crash_program` (Tests/CrashProgram.cpp) is the only thing that crashes on request; no
shipped program has a way to. `--inject-device-lost` doesn't crash: it runs the recovery
path, and a packaged game refuses it.
