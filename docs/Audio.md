# Audio

The `Audio` runtime module provides resident WAV/Ogg clips, an SDL3 stereo mixer,
spatial `AudioSource` components, and an `AudioWorld` subsystem per Play session.
Both editor and player use it. It depends on Engine (including Assets/Platform),
never on Renderer internals. WAV decoding is SDL3; Ogg Vorbis uses a pinned,
hash-verified stb_vorbis source in `cmake/AudioCodecs.cmake`.

## Editor

Save `.wav` or `.ogg` files into your project's `Content/` folder. Names are paths
relative to Content, such as `Audio/footstep.wav`. In the Content browser:

- Open the Audio folder; each clip tile has a Play/Stop audition button.
- Drag a clip into the viewport to create a named entity with Transform and AudioSource.
- Alternatively use its context menu to place a source or audition it.
- In an AudioSource inspector, choose a clip or drop a clip onto the Clip field.
  Tune the registered properties and use Preview/Stop preview to audition it.

Preview bypasses spatial positioning so it works without a listener. Only one preview
plays at a time; starting/stopping Play or switching projects stops it. Preview does
not modify the scene or undo history. Creating a source and editing fields do.
Audio configuration errors appear beneath the component. Tools can open these UI paths
reproducibly with `editor Games/Sample/Sample.swproject --content-folder Audio --select 15`
and `--preferences`; add `--frames N` for bounded previews.

Preferences → Audio → Master volume controls preview and Play. It is stored as
`master_volume = 0..1` in the readable per-user `editor.swprefs` file (see
`Preferences.h` for its XDG/default path), rather than in a scene or project.

## Scenes and validation

AudioSource is registered through `registerComponent`; fields are described in the
generated [SceneSchema](SceneSchema.md), with the project's clip names listed below
its component reference. The human inspector and text format use the same metadata.

```text
entity "Ambient sound"
  AudioSource
    clip "Audio/ambient.wav"
    volume 0.25
    pitch 1
    loop 1
    play_on_start 1
    spatial 0
    min_distance 1
    max_distance 30
end
```

A spatial source also needs a Transform. Volume is 0–1; pitch is 0.125–8 and affects
both speed and frequency. Loop repeats a clip continuously. Play on start starts
new sources when the session synchronizes them. Min distance is the full-volume
radius; volume falls linearly to silence at max distance. Equal min/max distances
form a hard cutoff. Both distances must be finite, nonnegative, with max ≥ min.

The listener uses the primary Camera's world position and right axis, including its
parents. Spatial clips are downmixed to mono, then use equal-power stereo pan.
Without a primary Camera with a Transform, spatial sources are silent. Nonspatial
clips preserve their stereo channels and do not need a Transform or listener.
This is stereo positioning, not HRTF or acoustic obstruction.

```sh
build/bin/player --check Games/Sample/Sample.swproject
build/bin/player --schema docs/SceneSchema.md
build/bin/player --input-schema docs/InputSchema.md
build/bin/player Games/Sample/Sample.swproject --audio-volume 0.5 --frames 120
```

`--check` rejects missing/empty clip names, failed imports, volume outside 0–1,
pitch outside 0.125–8, invalid distances, and spatial sources without Transform.
It uses the same audio validation function as the inspector. Player startup also
rejects invalid AudioSource configuration. The player's `--audio-volume` controls
its master gain; an invalid argument fails before engine initialization.

## Game API and lifecycle

Include `<Audio/Audio.h>`. During Play, `ctx.audio()` or
`ctx.subsystem<AudioWorld>()` gives the session's audio service:

```cpp
// A one-shot with no scene entity (for a menu action or the sample's pulse action).
auto voice = ctx.audio().playClip("Audio/pulse.wav", {.volume = 0.6f});
ctx.audio().mixer().setVolume(voice, 0.4f);
ctx.audio().mixer().stop(voice);

// A synchronized AudioSource entity, including its spatial settings.
ctx.audio().play(sourceEntity);
ctx.audio().setVolume(sourceEntity, 0.5f);
ctx.audio().stop(sourceEntity);
```

These transport calls are safe from any thread while the session is alive. They
post bounded commands and report failure (voice 0 / false) if unavailable, invalid,
or the command queue is full. `isPlaying(voice)` observes the mixer after commands
are consumed; it need not turn true immediately after play. Runtime setVolume on an
entity persists until its authored volume changes. Stop does not automatically
restart a play-on-start source; call play explicitly to replay it.

AudioWorld synchronizes on the main thread around gameplay, snapshots the listener
and sources after updates, and passes immutable PCM/shared playback settings to the
mixer. The SDL callback mixes off the main thread and never reads the World, loads a
file, or waits for gameplay's command mutex. Steady-state mixing uses fixed voice,
command, and output buffers. There are 128 voices and 1024 queued commands; when all
voices are occupied, a new play replaces the oldest. Sources removed from the World
stop at synchronization. Don't retain a subsystem pointer after Stop.

Play initializes a fresh AudioWorld before GameMode beginPlay and synchronizes again
after it; Pause produces silence without advancing playback positions, Resume
continues, and Stop closes the session's device and discards its sounds before
restoring the authored scene. CPU-only sessions without Engine have a silent offline
mixer. A missing physical audio device logs a warning and leaves gameplay usable.

## Imports, reload and tools

Clips decode/resample to immutable 48 kHz interleaved stereo float PCM. Initial
project import completes before validation/Play; later audio imports and reloads are
background jobs, published between frames. A successful reload changes active
entity sources at their current sample position. An already-playing one-shot started
with playClip or a preview keeps the immutable version it started with; new plays
use the replacement. A failed save retains the last-good version and reports an
import error; another save retries it. Removing a clip invalidates its asset name.

For audio-only tooling without a window/GPU, construct `AssetFolder(assets, jobs)`;
this imports audio but reports an error if asked to publish image/model GPU assets.
`loadAudioClip` decodes a single file, and `AudioMixer(false).render(stereoBuffer)`
provides deterministic offline mixing. Do not call render on a live device mixer.
These public APIs allow custom exporters, waveform tools, and PCM verification.

Clips are fully resident, not streamed music. Source and decoded PCM are each limited
to 256 MiB per clip; Ogg supports mono/stereo. Pitch uses linear sample interpolation.
There is no Doppler, reverb, occlusion, streaming, or dedicated audio graph in this
first module. Overlapping loud clips can saturate the mix, which clamps to −1..1.
WAV and Ogg are standard documented formats; no proprietary audio container is added.

## Sample and tests

`Games/Sample/Tools/GenerateAudio.py` reproduces the short, original CC0 ambient and
pulse WAVs from mathematical signals. The main scene loops the ambient; pressing
its `pulse` input plays the pulse alongside the existing flash/rumble. The Ogg test
fixture is an encoded copy of the generated pulse, also CC0. To regenerate that
fixture with the optional external conversion tool:

```sh
python Games/Sample/Tools/GenerateAudio.py
ffmpeg -i Games/Sample/Content/Audio/pulse.wav -c:a libvorbis Tests/fixtures/audio/pulse.ogg
SDL_AUDIO_DRIVER=dummy ctest --test-dir build --output-on-failure
```

AudioTest covers WAV/Ogg decoding, corrupt input, background reload/last-good retention,
scene round-trip/schema, checker rules, pan/falloff, pitch/loop/volume/PCM results,
worker transport, entity/listener updates, session cleanup, and the actual SDL callback.
It uses SDL's dummy audio driver, so CI needs no speakers. With xvfb-run installed,
CTest also invokes the real player checker on valid and invalid AudioSource scenes.
