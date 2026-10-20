# The Shockwave Book

A gentle, start-to-finish guide to how Shockwave works and how to make a game with it.

The [Tutorial](../Tutorial.md) gets a cube moving in about 20 minutes. This book goes
slower and further. Each chapter explains one idea in plain words, shows where it lives
in the code, and adds one piece to a small game called **Orb Hunt**. By the end you'll
have a finished, packaged game, and you'll understand what happens inside the engine
every frame.

You don't need to know game engines already. You do need to be comfortable reading a
little C++, and running commands in a terminal.

## How to read it

- Read the chapters in order the first time. Each one builds on the last.
- Every chapter starts with **In this chapter** and ends with **Recap**. If a recap
  already makes sense to you, skip ahead.
- Boxes marked **Under the hood** explain how the engine does something. You can skip
  them and still make your game.
- The other pages in `docs/` are the reference. The book links to them when you want
  every detail.

## Contents

### Part 1: Getting your bearings

1. [What a game engine is](01-what-is-a-game-engine.md)
2. [Build it and take a tour](02-build-and-tour.md)
3. [Projects and their files](03-projects-and-files.md)

### Part 2: Making Orb Hunt

4. [Entities and components](04-entities-and-components.md)
5. [Your first game code](05-first-game-code.md)
6. [Systems: work on every core](06-systems.md)
7. [What happens in one frame](07-one-frame.md)
8. [Physics: solid ground and a walking player](08-physics.md)
9. [Prefabs: build once, spawn many](09-prefabs.md)
10. [Sound](10-sound.md)
11. [A HUD with the score](11-game-ui.md)
12. [Saving the game](12-saving.md)

### Part 3: How the engine is built

13. [Drawing the picture: the renderer](13-rendering.md)
14. [Changing code while it runs](14-hot-reload.md)
15. [Inside the editor: panels, docking and themes](15-inside-the-editor.md)
16. [Command-line tools and checks](16-tools-and-checks.md)
17. [Shipping your game](17-shipping.md)
18. [Map of the code, and a glossary](18-code-map-and-glossary.md)

For renderer development, continue with the [Graphics implementation guide](../Graphics/README.md).
It is a planned-feature specification, with equations, UI/CLI contracts and verification
gates; it is separate from this book's explanation of existing behavior. The editor's
Help menu opens the guide and its individual chapters.

## The game you'll make

Orb Hunt is small on purpose. You walk a character around an arena. Glowing orbs float
and spin around you. Walk into one to collect it, hear a chime, and watch the score go
up. Press F5 to save and F9 to load. It's just enough to touch every part of the
engine without getting lost in game design.
