# Reactor Run playtest

For a person playing the packaged or locally built game for the first time. It takes
about ten minutes. Note anything that surprised you, and the step it happened at.

The automated version of this script is `Tests/flow/full-run.json` (keyboard and mouse),
`Tests/flow/gamepad-run.json` (gamepad) and `Tests/flow/resume-quit.json` followed by
`Tests/flow/resume-continue.json` (save, quit and continue), run by `shipping_demo_flow`.

## Before you start

- Delete or rename your Reactor Run saves so New game starts clean (the player's
  `--user-dir` folder, or the OS's save folder for the game).
- Have a gamepad nearby for step 9.

## The script

1. Start the game. **Expect** the main menu with "Reactor Run", New game, Continue
   greyed out with a reason, Settings, Credits and Quit.
2. Choose **New game**. **Expect** a loading screen, then the training corridor, with
   the objective "Walk to the firing line" and the controls along the bottom.
3. Walk to the yellow line. Shoot the three targets. **Expect** each to break after
   two hits, a hit sound, the crosshair changing over a target, and the ammo count
   going down. When the magazine empties, it reloads by itself.
4. Pick up the yellow ammo box by walking over it. Walk to the console on the right.
   **Expect** the prompt "[E] Open the gate" (or the button your controls use). Press it.
   **Expect** the gate to open.
5. Walk through the exit. **Expect** a loading screen and the factory.
6. Two sentries come at you and shoot. Take them out. **Expect** your health to drop
   when hit, a red flash, and the security door to open when both are down.
7. Walk on past the door. **Expect** "Checkpoint reached" in the top right.
8. Press **Escape** and choose **Main menu**. **Expect** Continue to be available now.
   Quit the game completely, start it again, and choose **Continue**. **Expect** to be
   back in the factory at the checkpoint, with the sentries still gone.
9. Pick up the gamepad. **Expect** the prompts to change to its buttons as soon as you
   use it. Walk to the reactor console and use it.
10. Hold the reactor room for eight seconds while the guard attacks. Then walk to the
    green exit at the end and use it. **Expect** the results: time, takedowns, shots and
    accuracy. Choose **Continue**.
11. **Expect** the ending and its credits. Choose **Back to the main menu**.
12. Choose **New game**. **Expect** it to ask first, then to start training afresh.

## Also try

- Go down on purpose in the factory: **expect** Restart from checkpoint, Restart level
  and Main menu, each doing what it says.
- Rebind Use in `Config/Input.swinput` (say to `key F`) and restart: **expect** the
  prompt to say F.
