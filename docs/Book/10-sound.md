# 10. Sound

**In this chapter:** placing sounds in a scene, playing them from code, and where the
listener is.

## Sounds are assets

Put `.wav` or `.ogg` files anywhere in `Content/`. Like every asset, a sound is named by
its path inside Content. For Orb Hunt, borrow the sample's sounds:

```sh
mkdir -p Games/OrbHunt/Content/Audio
cp Games/Sample/Content/Audio/pulse.wav Games/OrbHunt/Content/Audio/chime.wav
cp Games/Sample/Content/Audio/ambient.wav Games/OrbHunt/Content/Audio/ambient.wav
```

In the editor, each sound in the Content panel has a play button, so you can listen
before you use it.

## Background sound in the scene

A sound that plays by itself is an entity with an `AudioSource`. Add one to
`arena.swscene`:

```text
entity "Ambience"
  AudioSource
    clip "Audio/ambient.wav"
    volume 0.3
    loop 1
    play_on_start 1
    spatial 0
end
```

`spatial 0` means it isn't placed in the world: it plays the same in both ears, like
music. It doesn't even need a Transform.

## A spatial sound

Set `spatial 1` and give the entity a Transform, and the sound comes *from* that spot.
It gets quieter between `min_distance` and `max_distance`, and moves between your left
and right ear as you turn. With headphones, Shockwave uses Steam Audio's HRTF, which
also lets you hear whether a sound is above, below or behind you, and walls can muffle
it. [Spatial audio](../SpatialAudio.md) covers all of that.

Who's listening? The scene's primary Camera, unless some entity has an `AudioListener`
component. For a third-person game like Orb Hunt you might put an `AudioListener` on
the Player, so sounds are heard from where the character stands.

## Play a sound from code

In `collectOrbs`, play the chime for each orb collected. In the loop at the end:

```cpp
        for (Entity e : collected) {
            game.world.destroy(e);
            spawnOrb(game);
            game.audio().playClip("Audio/chime.wav", {.volume = 0.6f});
        }
```

`playClip` returns at once; the sound plays in the background. The options struct
(`AudioPlayback`, in `Audio/Audio.h`) also has `pitch`, `loop`, and settings for playing it from a position in the world.

Rebuild and play. Each orb now chimes.

> **Under the hood.** Sound is the `Audio` module. Mixing runs on its own thread
> (through SDL3), so game code never waits for it. At the end of each tick, the Play
> session hands the mixer a snapshot of the listener and every source. The mixer reads
> that snapshot while the game carries on with the next tick, so the two never touch
> the same data at the same time.

## Recap

- Sounds are files in Content, named by their path.
- An `AudioSource` plays from the scene; `spatial 1` places it in the world.
- `game.audio().playClip("Audio/x.wav")` plays one from code.
- The listener is the primary Camera, or an entity with `AudioListener`.

Next: [A HUD with the score](11-game-ui.md).
