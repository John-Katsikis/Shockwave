# Spatial audio

Spatial audio makes a sound seem to come from a place in your world. With headphones you
can tell left from right, front from behind and up from down. Sounds also get quieter and
duller with distance, can face a direction, and are muffled by walls.

Shockwave renders spatial sound with Valve's [Steam Audio](https://valvesoftware.github.io/steam-audio/),
which is free and open source. If its library is missing, sounds fall back to simple
left and right panning, so a game never breaks because of it. For the basics of adding
sounds, read [Audio](Audio.md) first.

## Try the demo

```sh
./build/bin/editor Games/Sample/Scenes/spatial-audio.swscene --play
```

This opens the demo scene and presses Play. Put on headphones. The blue pulse is on your
left, the orange hum is behind a wall on your right, and the yellow horn in front of you
faces away. Move the camera round to hear them change.

## Make a sound 3D

Any `AudioSource` with Spatial turned on is a 3D sound. It needs a `Transform`, which says
where it is. To add one:

- **Create > Sound source** adds a looping 3D sound in front of the viewport camera. Pick
  its clip in the Inspector.
- Or drag a sound from the Content browser into the viewport.

Select a sound to see two rings round its speaker icon in the viewport. Inside the solid
ring (Min distance) it plays at full volume. At the faint ring (Max distance) it has faded
to silence. A directional sound also shows a line for the way it faces.

In a scene file, a 3D sound looks like this:

```text
entity "Campfire"
  Transform
    position 3 0.5 -2
  AudioSource
    clip "Audio/fire.ogg"
    loop 1
    spatial 1
    min_distance 1
    max_distance 20
    rolloff Inverse
end
```

Fields you leave out keep their defaults.

## Choose who listens

The listener is the pair of ears that hears 3D sound. It looks down its entity's -Z axis,
the same way a camera does.

- By default, the scene's primary Camera listens.
- To listen from somewhere else, such as a character's head in a third-person game, add an
  `AudioListener` component to that entity. **Create > Audio listener** adds one where the
  viewport camera is.
- If a scene has several listeners, the first one in the scene file wins. The others say
  "Not used" in the Inspector, and `player --check` reports them.

The listener shows as a headphones icon in the viewport. The one that's in use is drawn
brighter.

## Spatial settings

These are on `AudioSource`, below the settings described in [Audio](Audio.md). They only
matter when Spatial is on.

| Setting | Text name | Default | What it does |
|---|---|---|---|
| Rolloff | `rolloff` | `Linear` | How volume falls between min and max distance. `Linear` is a straight line. `Inverse` is like real sound: it drops fast near the source, then slowly. `None` stays at full volume, then cuts off at max distance. All three reach silence at max distance. |
| Air absorption | `air_absorption` | on | Distant sounds lose their high notes, like thunder far away. |
| Directivity | `directivity` | 0 | 0 sounds the same in every direction. 0.5 is like a voice: quieter behind. 1 is loudest in front (-Z) and silent behind. |
| Directivity power | `directivity_power` | 1 | Higher numbers narrow the beam, like a megaphone. 0 to 16. |
| Occlusion | `occlusion` | off | Walls between the sound and the listener muffle it. |
| Occlusion radius | `occlusion_radius` | 0.5 | How big the sound is, in meters, when checking for walls. A bigger sound is only partly hidden by a thin post. 0 checks one line. |
| Transmission | `transmission` | 0.1 | How much sound gets through a wall. Low notes get through more than high ones. 0 to 1. |

`Inverse` rolloff needs a min distance above 0.

### Muffle sounds behind walls

Turn on Occlusion. Shockwave then casts a few rays from the sound to the listener, against
the scene's physics colliders. Any entity with a `Collider` blocks sound, so walls need a
`Collider` (see [Physics](Physics.md)). The sound's own collider, its parent's collider and
the listener's don't count.

The result fades in over about an eighth of a second, so a sound doesn't click when you
step behind a pillar. Each occluded sound casts 5 rays per tick, or 1 with a radius of 0.
Sounds past their max distance cast none.

## Headphones or speakers

Open **File > Project settings**:

| Setting | Text name | What it does |
|---|---|---|
| Spatial audio | `audio_hrtf` | `true` (the default) is for headphones. It uses an HRTF, a model of how your head and ears shape sound, so you can hear front, back, up and down. `false` is for speakers: sounds are panned between them. |
| HRTF quality | `audio_hrtf_quality` | `high` (the default) blends smoothly between directions. `low` snaps to the nearest measured direction and costs a little less. |

The editor uses the new setting in Play straight away, and Save writes it to the
`.swproject` file:

```text
audio_hrtf false
audio_hrtf_quality low
```

These lines are only written when they differ from the defaults. In the player,
`--audio-hrtf on` or `--audio-hrtf off` overrides the project for one run.

## Play 3D sounds from game code

For a gunshot, an explosion or a footstep, you don't need an entity. Play the sound at a
point, or attached to an entity:

```cpp
#include <Shockwave/Game.h>
using namespace shockwave;

// At a point. It stays there until it ends.
game.audio().playAt("Audio/boom.wav", hitPoint);

// On an entity. It follows the entity until it ends.
AudioVoice engine = game.audio().playOn("Audio/engine.wav", game.find("Car"), {.loop = true, .maxDistance = 60});
game.audio().stopVoice(engine);
```

The last argument is an `AudioSource`, so it takes every setting from the table above,
such as `.volume`, `.pitch`, `.rolloff` or `.occlusion`. Its clip is ignored.

Things to know about these calls:

- They are safe from any thread while Play is running.
- They return 0 when the clip is missing, a setting is invalid, or all 128 one-off 3D
  sounds are busy.
- A looping sound from `playOn` stops when its entity is destroyed. A one-shot keeps
  playing where the entity last was.

Sound source entities in the scene work as in [Audio](Audio.md): `game.audio().play`,
`stop` and `setVolume`.

## See what you're hearing

During Play, select a sound. The Inspector shows how the listener hears it, for example
"Heard 4.2 m away to the left, volume 61%", and how many occlusion rays get through.

**Window > Stats** has an Audio section. It shows what renders spatial sound, how many
voices play, how many go through Steam Audio, and what mixing costs.

## Hear a scene from the command line

`player --audio-mix` plays a scene with no speakers needed, writes what you would hear to
a WAV file, and prints where each sound is. Tools and AI agents can use it to check audio
without listening.

```sh
./build/bin/player Games/Sample/Sample.swproject --scene Games/Sample/Scenes/spatial-audio.swscene --audio-mix mix.wav --seconds 2
```

You'll see output like this:

```text
audio mix: Scenes/spatial-audio.swscene, 2.00 s, 48000 Hz stereo, Steam Audio HRTF
listener: 'Player' (AudioListener) at 0.00 1.60 4.00
source 'Pulse (left)' (Audio/pulse.wav): 4.04 m, azimuth -90 deg (left), elevation -9 deg, gain 0.208, air high 0.929, occlusion 1.00, playing
source 'Hum (behind the wall)' (Audio/ambient.wav): 5.04 m, azimuth +90 deg (right), elevation -7 deg, gain 0.832, air high 0.912, occlusion 0.00, playing
source 'Horn (faces away)' (Audio/pulse.wav): 10.02 m, azimuth +0 deg (ahead), elevation -3 deg, gain 0.000, air high 0.833, occlusion 1.00, playing
channel  rms       peak      rms dBFS
left     0.02652   0.12683   -31.5
right    0.01581   0.08078   -36.0
balance: left louder by 4.5 dB
mix cost: 28.4 us per 256-frame block (0.53% of one core), peak 55.1 us; up to 3 voices, 3 through Steam Audio
Wrote mix.wav
```

Here the hum is fully behind the wall (occlusion 0.00), so only a little of it gets
through, and the horn faces away, so its gain is 0. That's why the left side is louder.

| Option | What it does |
|---|---|
| `--audio-mix out.wav` | Plays the scene and writes a 16-bit stereo WAV. |
| `--seconds N` | How long to play, up to 600. The default is 3. |
| `--scene file` | Which scene. The default is the project's startup scene. |
| `--audio-hrtf on` or `off` | Headphones or speakers, for this run. |

Azimuth is the angle round you: 0 is ahead, -90 is left, +90 is right and 180 is behind.
Gain is the volume from distance and facing, before walls.

## Check a scene for spatial audio mistakes

```sh
./build/bin/player --check Games/Sample/Sample.swproject
```

On top of the checks in [Audio](Audio.md), this reports:

- a spatial setting out of range, such as directivity above 1
- `Inverse` rolloff with a min distance of 0
- occlusion turned on for a sound that isn't spatial
- an `AudioListener` without a `Transform`
- more than one `AudioListener`
- spatial sounds with nothing to hear them: no listener and no primary Camera

The Inspector shows the same problems under the component.

## When Steam Audio is missing

The build downloads Steam Audio 4.8.1 and copies its library, `libphonon.so` on Linux,
into `build/bin` next to the editor and player. It's loaded when the first sound plays.

If it can't be found, or `SHOCKWAVE_PHONON=none` is set, spatial sounds use simple
equal-power panning with the same distance, facing and occlusion volume. There is no HRTF
and no air absorption then. The editor's Stats window and `--audio-mix` say which one you
got, and why.

| Setting | Where | What it does |
|---|---|---|
| `SHOCKWAVE_STEAM_AUDIO` | CMake option, on by default | Off builds without Steam Audio. |
| `SHOCKWAVE_PHONON` | Environment variable | A path to the library to load, or `none` to force simple panning. |

Steam Audio is under the Apache 2.0 license. Its license and the notices for the libraries
inside it are copied to `build/bin/licenses/SteamAudio`.

## What it costs

Spatial mixing runs on the audio thread, not on the frame, so it doesn't change your
frame rate or frame pacing. Measured with `spatial_audio_test` on a 2.1 GHz Xeon cloud CPU
(one core, Release build), per sound:

| Mode | Cost per sound | Share of one core |
|---|---|---|
| HRTF, high quality | about 11 microseconds per 5.3 ms block | about 0.2% |
| HRTF, low quality | about 7 microseconds | about 0.12% |
| Speaker panning | about 3 microseconds | about 0.06% |
| Simple panning (no Steam Audio) | about 2 microseconds | about 0.04% |

So 64 sounds with high quality HRTF cost about 0.7 ms per block, about 13% of one core.
Occlusion costs up to 5 physics rays per occluded sound per tick, on the main thread.
A modern desktop CPU is usually faster than this cloud machine.

The audio thread never allocates memory, loads a file or waits for the game. All 64 Steam
Audio voices are made when Play starts. Window > Stats shows the live mix cost.

## Limits

- Up to 64 sounds at once go through Steam Audio. Extra 3D sounds play with simple panning.
- Occlusion uses physics colliders, not the rendered meshes. Every collider blocks the
  same amount; there are no per-material settings yet.
- There is no reverb, sound bouncing round rooms, or Doppler yet. Steam Audio supports
  reflections, so they can be added later.
- Steam Audio ships prebuilt libraries for Linux x64, Windows x64 and macOS. On other
  platforms spatial sound uses simple panning.
- Sound is mixed down to mono before it's placed in 3D, as in most engines.

## How it works

On the main thread, once per tick, `AudioWorld::update` casts occlusion rays and smooths
them. Then `AudioWorld::synchronize` works out, for each sound, its direction from the
listener, distance volume, facing and air absorption (`spatialize` in
`Audio/SpatialAudio.h`). It sends those numbers to the mixer as a small command.

On the audio thread, each 3D voice gets a Steam Audio slot. The mixer renders all of them
in fixed blocks of 256 samples. For each voice, a direct effect applies distance, air,
facing and walls, and then a binaural effect (HRTF) or a panning effect places it. The
result is added to the normal mix.

The engine doesn't link Steam Audio. It opens the library at runtime and looks up the
functions it needs, which is why a missing library only means simpler sound.

`Tests/SpatialAudioTest.cpp` checks the math, the checker's rules, Steam Audio's output,
the fallback, occlusion rays, one-off sounds, listeners and the cost per voice, with no
speakers. CTest runs it twice: with Steam Audio, and with `SHOCKWAVE_PHONON=none`.
