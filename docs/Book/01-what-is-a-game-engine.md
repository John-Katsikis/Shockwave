# 1. What a game engine is

**In this chapter:** what an engine does for you, what Shockwave is made of, and the
three programs you'll use.

## The job of an engine

Every game, from Tetris to a huge open world, does the same thing over and over, many
times a second:

1. **Look at the controls.** Which keys are down? Where is the stick pointing?
2. **Move the world forward a little.** Characters walk, things fall, timers tick.
3. **Draw a picture of the world** and show it on the screen.

Each pass through those three steps is called a **frame**. At 60 frames per second the
whole loop happens every 16 milliseconds.

You could write all of that yourself: open a window, talk to the graphics card, load
images, simulate physics, mix sound. It would take years. A game engine is all of that
work done once, so you only write the part that makes your game *your* game: the rules.

## What Shockwave gives you

Shockwave is a 3D engine written in C++20. Here's what it does for you:

| You get | Which means |
|---|---|
| A window and input | Keyboard, mouse, gamepads, wheels and flight sticks, all behind named actions like "jump" |
| A renderer | Draws your 3D scene with Vulkan: lights, shadows, models, textures, particles |
| Physics | Things fall, collide and bounce (Jolt Physics) |
| Sound | Clips that play from a place in the world, with 3D headphone sound (Steam Audio) |
| Game UI | Menus and HUDs written like web pages (RmlUi) |
| Animation and navigation | Characters that play animations and find their way around walls |
| An editor | A program for building scenes by clicking, and playing them instantly |
| Tools | Commands that check, screenshot, benchmark and package your game |

## The three programs

When you build Shockwave you get three programs in `build/bin`:

- **`editor`**: where you build scenes and press Play to try them. Most of your time is
  spent here.
- **`player`**: runs your game on its own, the way players will see it. It's also a
  toolbox: `player --check` finds mistakes, `player --screenshot` takes pictures,
  `player --package` makes a folder you can give to people.
- **`sandbox`**: a small test program for engine developers. You can ignore it.

Here's the important part: **the editor and the player run the same engine.** They both
create one `Engine` object and let it drive the loop. They only differ in what they do
with each frame. The editor draws panels and lets you edit; the player just runs the
game. So what you see when you press Play in the editor is what your players get.

> **Under the hood.** The engine is the `Engine` class in
> `Source/Runtime/Engine/Public/Engine/Engine.h`. A program plugs into it by deriving
> from `Application` and filling in a few hooks: `onStart`, `onUpdate`, `onFrame`,
> `onGui`, `onRender`, `onShutdown`. The editor's `Editor` class and the player's
> `Player` class are both `Application`s. Chapter 7 walks through the loop line by line.

## Where Shockwave borrows its ideas

If you've used Unity or Unreal, Shockwave will feel familiar, because it borrows names
from Unreal: **GameMode**, **GameInstance**, **Subsystem**, **Play In Editor**. Inside,
though, it's built differently. It uses an **ECS**, short for *entity component
system*. Chapters 4 to 6 explain what that means. The short version: your game's data
lives in small plain structs, and functions called *systems* process that data in
parallel across all your CPU cores.

## Recap

- A frame is: read input, update the world, draw it. Games repeat it 60 or more times
  a second.
- An engine does everything except your game's rules.
- You'll use the **editor** to build and the **player** to run and check.
- Both run the same `Engine`, so Play in the editor matches the real game.

Next: [Build it and take a tour](02-build-and-tour.md).
