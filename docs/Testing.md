# Testing

Shockwave's tests are CTest tests. Build, then run them all:

```sh
cmake --build build
ctest --test-dir build -j6 --output-on-failure
```

Headless runs need `SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy` (the tests that
need it set it themselves).

## Labels

Every test has a label that says what it needs, so a machine can run what it can:

| Label | What it needs |
|---|---|
| `unit` | Nothing but the program: no window, GPU or game package. |
| `integration` | The player or editor on real projects, often a game module and a window (offscreen). |
| `gpu` | A Vulkan device. Mesa's software Vulkan counts in CI; a hosted runner without one can't. |
| `timing` | A machine that keeps time well: frame pacing and GPU timings. Shared runners oversleep. |
| `installed` | A package made, installed and played (`player --verify-install`). |
| `online_mock` | The online services, mocked (the Steam mock, SDR transport mock). |
| `online_real` | Real Steam. None yet; these will need a signed-in Steam client. |

A test can have more than one (`installed_game_flow` is `integration;installed`). Pick with
CTest's `-L` and `-LE`:

```sh
ctest --test-dir build -L unit                 # the quick ones
ctest --test-dir build -LE "gpu|timing"        # everything a GPU-less runner can do
```

Every test also has a `TIMEOUT`. Configuring stops on a test without a timeout or with no
label, or a label not in the table: each folder with tests ends with
`cmake_language(DEFER CALL shockwave_check_tests)` (the function is in the top
`CMakeLists.txt`). `-DSHOCKWAVE_TEST_TIMEOUT_SCALE=5` multiplies every timeout, for slow
sanitizer builds.

## The CI gate

CI never runs a bare `ctest -R` or `-L`: a filter that matches nothing passes, so a renamed
test or a changed label would quietly stop being tested. `cmake/CTestGate.cmake` runs the
same filter in two steps. It lists what the filter selects (`ctest --show-only=json-v1`) and
refuses it when that's nothing, fewer than expected, or missing a test it must include.
Only then does it run exactly those, with `--no-tests=error`:

```sh
cmake -DBUILD=build -DLABELS=unit -DEXPECT=40 -DREQUIRE="install_records" -P cmake/CTestGate.cmake
cmake -DBUILD=build -DEXCLUDE_LABELS="gpu|timing" -DEXPECT=90 \
    -DREQUIRE_FILE=Tests/RequiredTests.txt -DPLATFORM=macos -P cmake/CTestGate.cmake
```

It also takes `-DTESTS` / `-DEXCLUDE_TESTS` (regexes), `-DCONFIG` (Windows' multi-config
builds), `-DREPEAT=until-fail:5`, `-DPARALLEL` and `-DNAME`. In GitHub Actions it writes what
it selected, how many were expected and the result to the job summary.

`Tests/RequiredTests.txt` lists the tests each platform's build must define at all, whatever
the job's filter: the shipping plan's inventory and the long-standing checks. A name alone is
required everywhere; platform names after it (`linux`, `macos`, `windows`) limit it. Add a
test there when CI must never lose it.

## CI

`.github/workflows/build.yml` runs on every push and pull request:

- **build** (Linux): every test, the GPU ones too on Mesa's software Vulkan; the sample, the
  shipping demo and Arena pass `--check` and stay unchanged; every generated reference
  (scene, input, circuit nodes, graphics, lighting, materials, post-processing, levels,
  preferences, release, network) regenerates byte for byte.
- **macos** and **windows**: every test that needs no GPU (and on macOS no stopwatch). The
  GPU tests run afterwards as advisory: they report but don't decide the job, since hosted
  runners may have no usable Vulkan.
- On all three, `installed_game_flow` packages Reactor Run, installs it the hard way, plays,
  updates it from 1.0.0 to 1.1.0, goes back, and reinstalls it. Each job keeps the install
  records as an artifact (`install-records-<os>`). They're CI-only evidence: played on the
  null renderer, they prove the install, not the graphics, so they never pass a platform's
  release gate ([Release](Release.md#platforms)).

`.github/workflows/nightly.yml` runs every night, on demand (Actions > Nightly > Run
workflow), and on a pull request that changes it, the gate or the shipping tutorial:

- **asan-ubsan**: a RelWithDebInfo build with `-DSHOCKWAVE_SANITIZE=address,undefined` runs
  every test without a GPU, a stopwatch or a package (a sanitized program needs the
  sanitizer's runtime, which packaging rightly refuses to ship). Leaks count. The two
  crash-handler tests (`diagnostics_cli`, `failure_recovery`) sit this one out: the
  sanitizer installs its own handlers for the same signals. The regular jobs run them.
- **tsan**: `-DSHOCKWAVE_SANITIZE=thread` over the parts that run on several threads: jobs,
  streaming, level lifetime and travel, processes, the transport and sessions, diagnostics.
  See [Third-party code](#third-party-code) for what it doesn't check.
- **repeated**: travel, Stop and saves five times over; the cooks and the installed game
  three times; every network fault preset for two minutes of play with that night's seed
  (`arena_network_test Games/Arena --soak all 120 --seed N` replays a failure); the network
  fault tests three times.
- **tutorial**: follows [the shipping tutorial](ShippingTutorial.md) word for word into a
  new folder outside the checkout, with only that game registered:
  `python3 Tests/FollowTutorial.py docs/ShippingTutorial.md /new/folder/TwoRooms`. Every
  file it writes and command it runs comes from the Markdown, and each expected output is
  checked, ending with `--verify-install` of the release package.

Release candidates need more than CI can give: real GPUs on each supported hardware tier,
real Steam, the platform records from real machines, and a [rehearsal](ReleaseRehearsal.md)
with a new tester. Those are recorded evidence in `Release/`, gathered as
[Release](Release.md) describes.

## Third-party code

The sanitizers check Shockwave's own code fully. A few findings in dependencies we build but
don't own are switched off narrowly, each with its reason beside it:

- **basisu** reads bits through deliberately unaligned pointers (`-fno-sanitize=alignment`).
- **Recast/Detour** hash with int overflow on purpose (`-fno-sanitize=signed-integer-overflow`).
- **SDL3** copies zero display modes from a null array when a display has none
  (`-fno-sanitize=nonnull-attribute`).
- **Jolt** frees jobs after an acquire fence, which TSan can't see; TSan builds define
  `JPH_TSAN_ENABLED` so Jolt uses its acq_rel path instead.
- `Tests/lsan.supp`: SDL3's per-thread error buffer on threads it didn't create (the job
  workers), and what its X11 backend keeps from video init past `SDL_Quit`.
- `Tests/tsan.supp`: SDL3's HIDAPI libusb backend stopping its event thread through a
  plain flag, and libdbus taking its own two locks in opposite orders at SDL's init and quit.

The flags are set in the top `CMakeLists.txt` (only with `SHOCKWAVE_SANITIZE`). A new entry
needs a finding traced into that library's code and a reason; a finding in Shockwave's code
gets fixed, not suppressed.

Time budgets inside otherwise functional tests (`test_snapshot_delta`'s per-tick cost) are
stretched ten times in sanitizer builds, not dropped.

## Sanitizer builds at home

```sh
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSHOCKWAVE_SANITIZE=address,undefined -DSHOCKWAVE_TEST_TIMEOUT_SCALE=5
cmake --build build-asan
LSAN_OPTIONS=suppressions=$PWD/Tests/lsan.supp ctest --test-dir build-asan \
    -LE "gpu|timing|installed" -E "^package|^cook_|^diagnostics_cli$|^failure_recovery$"
```

GCC and Clang only. For `thread`, newer kernels may need `sudo sysctl vm.mmap_rnd_bits=28`.
