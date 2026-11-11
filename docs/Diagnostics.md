# Diagnostics

When a game misbehaves on a player's machine, Shockwave keeps what's needed to understand
it later: a log file, a crash report with the moments before the crash, and a note of
whether the last run ended cleanly. All of it stays on that machine, in the player's own
folders ([UserPaths.md](UserPaths.md)). Nothing is ever uploaded.

This is S12 part 1 of the shipping plan. Part 2 adds the in-game "last run crashed" prompt,
the device-lost restart path and safe process handling.

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

`crash_program` (Tests/CrashProgram.cpp) is the only thing that crashes on request; no
shipped program has a way to.
