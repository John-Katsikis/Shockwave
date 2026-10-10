# Audio

Shockwave can play WAV and Ogg Vorbis sounds, either from sound sources placed in a
scene or from your game code. Sounds can be positioned in the world, so they get quieter
with distance and pan between the left and right speakers. The editor and the player
share the same audio system.

New to Shockwave? [Tutorial: your first game in Shockwave](Tutorial.md) walks through a
whole project first.

## Add a sound to a scene

1. Save a `.wav` or `.ogg` file in your project's `Content/` folder. The sound's name is
   its path inside that folder, such as `Audio/footstep.wav`.
2. In the editor's Content browser, open the folder. Each sound has a Play/Stop button
   so you can listen to it.
3. Drag the sound into the viewport. This creates a named entity with a `Transform` and
   an `AudioSource` component.

You can also right-click a sound and use its menu to place a source or listen to it.

In the `AudioSource` Inspector, pick a clip or drop a sound onto the Clip field. Adjust
the settings below, and press Preview or Stop preview to hear the result.

## Sound source settings

| Setting | Text name | What it does |
|---|---|---|
| Clip | `clip` | The sound to play, by name. Must not be empty. |
| Volume | `volume` | 0 to 1. |
| Pitch | `pitch` | 0.125 to 8. Changes both speed and how high the sound is. |
| Loop | `loop` | Repeats the sound forever. |
| Play on start | `play_on_start` | Starts the sound as soon as Play picks up the source, including sources added during Play. |
| Spatial | `spatial` | Positions the sound in the world. Needs a `Transform`. |
| Min distance | `min_distance` | Inside this distance the sound is at full volume. |
| Max distance | `max_distance` | At this distance the sound fades to silence. |

Here is how a nonspatial looping background sound looks in a `.swscene` file:

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

Between min and max distance, volume falls in a straight line to silence. If both are
equal, the sound cuts off sharply at that distance. Both distances must be finite
numbers of 0 or more, and max must be at least min.

The full field list is in the generated [SceneSchema](SceneSchema.md). It also lists
your project's sound names below the component reference. The Inspector and the text
format use the same field descriptions.

## Who hears spatial sound

The listener is the scene's primary Camera. Its world position and its right-hand
direction set where sounds come from, including any parent entities it has.

Spatial sounds are mixed down to mono, then panned between left and right with
equal-power panning, which keeps loudness steady across the pan. If the scene has no
primary Camera with a `Transform`, spatial sounds are silent.

Nonspatial sounds keep their original stereo channels. They need neither a `Transform`
nor a listener. This is simple stereo positioning. There is no HRTF (3D headphone
audio) and nothing blocks sound.

## Preview sounds in the editor

Preview ignores position, so it works without a listener. A few rules:

- Only one preview plays at a time.
- Starting or stopping Play, or switching projects, stops the preview.
- Previewing doesn't change the scene or the undo history. Creating a source and
  editing its fields do.

If a source is set up wrong, the error appears beneath the component in the Inspector.

## Change the master volume

In the editor, go to Preferences, then Audio, then Master volume. It controls preview
and Play. It is saved as `master_volume = 0..1` in your personal `editor.swprefs` file,
not in a scene or project. `Preferences.h` says where that file lives on each system.

In the player, use `--audio-volume`:

```sh
build/bin/player Games/Sample/Sample.swproject --audio-volume 0.5 --frames 120
```

This runs the sample at half volume for 120 frames, then quits. An invalid value fails
before the engine starts.

## Check a scene for audio mistakes

```sh
build/bin/player --check Games/Sample/Sample.swproject
```

This checks the project's scenes and exits with an error if anything is wrong. For
audio it rejects:

- a missing or empty clip name
- a clip that failed to import
- volume outside 0 to 1, or pitch outside 0.125 to 8
- invalid distances
- a spatial source without a `Transform`

`--check` uses the same validation as the Inspector. The player also refuses to start a
scene with an invalid `AudioSource`.

These commands regenerate the reference docs:

```sh
build/bin/player --schema docs/SceneSchema.md
build/bin/player --input-schema docs/InputSchema.md
```

## Play sounds from game code

`<Shockwave/Game.h>` includes the audio API. During Play, `game.audio()` gives you the
session's audio service. See [Projects](Projects.md) for how game code is set up.

Play a one-off sound with no entity, for a menu click or a pickup:

```cpp
#include <Shockwave/Game.h>
using namespace shockwave;

auto voice = game.audio().playClip("Audio/pulse.wav", {.volume = 0.6f});
game.audio().mixer().setVolume(voice, 0.4f);
game.audio().mixer().stop(voice);
```

Control a sound source entity from the scene, with its spatial settings:

```cpp
if (EntityRef ambient = game.find("Ambient sound")) {
    game.audio().play(ambient.entity());
    game.audio().setVolume(ambient.entity(), 0.5f);
    game.audio().stop(ambient.entity());
}
```

`game.subsystem<AudioWorld>()` returns the same service as a pointer.

Things to know about these calls:

- They are safe from any thread while Play is running.
- They queue a command and return failure, a voice of 0 or `false`, if audio is
  unavailable, the input is invalid, or the queue is full.
- `isPlaying(voice)` may not turn true right after you call play. It reports what the
  mixer has done so far.
- Setting an entity's volume in code lasts until its authored volume changes.
- Stop doesn't restart a play-on-start source. Call play to hear it again.
- Don't keep a pointer to the audio service after Play stops.

## What happens on Play, Pause and Stop

- **Play** creates a fresh audio service before `GameMode::beginPlay` runs, and
  synchronizes the scene's sources again after it.
- **Pause** goes silent without moving any sound forward. **Resume** continues.
- **Stop** closes the audio device and throws away the session's sounds, then restores
  the authored scene.

If the computer has no audio device, a warning is logged and the game still runs. Tools
and tests that run without an `Engine` get a silent offline mixer.

## Reload sounds while you work

When you save a sound file, it reimports in the background and swaps in between frames.

- Sound source entities that are playing switch to the new version at their current
  position.
- One-off sounds from `playClip`, and previews, keep the version they started with. New
  plays use the new one.
- If the new file fails to load, the last working version stays and an import error is
  reported. Saving again retries.
- Deleting a sound file makes its name invalid.

The first import when a project opens finishes before validation or Play starts.

## Limits

- Sounds are loaded fully into memory. There is no streaming, so this isn't meant for
  long music tracks.
- Each clip is limited to 256 MiB of source file and 256 MiB of decoded audio.
- Ogg files can be mono or stereo.
- Pitch uses simple linear interpolation between samples.
- There is no Doppler, reverb, occlusion or audio graph yet.
- Several loud sounds at once can clip, since the mix is clamped to the range -1 to 1.
- Up to 128 sounds play at once. When all are busy, a new sound replaces the oldest.
- Up to 1024 commands can wait in the queue.

WAV and Ogg are standard, documented formats. Shockwave adds no custom audio format.

## Build audio tools

You can use the audio code without a window or GPU, for exporters, waveform tools or
tests:

- `AssetFolder(assets, jobs)` imports audio. It reports an error if asked to publish
  image or model GPU assets.
- `loadAudioClip` decodes one file.
- `AudioMixer(false).render(stereoBuffer)` mixes offline, with the same result every
  time. Don't call `render` on a mixer that is playing to a real device.

## Sample sounds and tests

`Games/Sample/Tools/GenerateAudio.py` rebuilds the sample's short ambient and pulse WAVs
from math. They are original and CC0 (public domain). The main sample scene loops the
ambient sound. Pressing its `pulse` input plays the pulse, along with the existing flash
and controller rumble.

The Ogg test file is an encoded copy of the pulse, also CC0. To rebuild the sounds and
the Ogg file, then run the tests with no speakers needed:

```sh
python Games/Sample/Tools/GenerateAudio.py
ffmpeg -i Games/Sample/Content/Audio/pulse.wav -c:a libvorbis Tests/fixtures/audio/pulse.ogg
SDL_AUDIO_DRIVER=dummy ctest --test-dir build --output-on-failure
```

The `ffmpeg` step needs ffmpeg installed. It is optional.

AudioTest covers:

- WAV and Ogg decoding, and corrupt files
- background reload and keeping the last working version
- saving scenes, the schema, and the checker's rules
- panning, distance falloff, pitch, looping, volume and the exact audio output
- commands from worker threads, entity and listener updates, and session cleanup
- the real SDL audio callback

It uses SDL's dummy audio driver, so CI machines need no speakers. If `xvfb-run` is
installed, CTest also runs the real player checker on valid and invalid `AudioSource`
scenes.

The tools can open audio screens in a repeatable way, for screenshots:

```sh
./build/bin/editor Games/Sample/Sample.swproject --content-folder Audio --select 15
```

This opens the Content browser at the Audio folder and selects the scene's 15th entity.
Add `--preferences` to open Preferences, and `--frames N` to quit after N frames.

## How it works

The audio module depends on the engine, assets and platform code, never on renderer
internals. SDL3 opens the stereo audio device and decodes WAV. Ogg Vorbis uses a copy of
stb_vorbis that is pinned and hash-checked in `cmake/AudioCodecs.cmake`.

Every clip is decoded and resampled to 48 kHz stereo float samples that never change
once made. Each Play session gets one `AudioWorld` subsystem, which owns the mixer.

On the main thread, around gameplay, `AudioWorld` takes a snapshot of the listener and
the sources after updates. It hands the mixer the decoded samples and playback
settings. Sources removed from the scene stop at that point.

SDL's audio callback mixes on its own thread. It never reads the scene, loads a file, or
waits for gameplay's lock. While running, it uses fixed-size buffers for voices,
commands and output.
