# Circuits

Circuits is Shockwave's visual scripting system, like Blueprints in Unreal. A circuit is a node
graph saved as a `.swcircuit` file in Content. You can make game logic with one instead
of writing C++: open a door when the player walks in, spin a pickup, count a score.

White **exec** wires say what happens and in which order. Every wire starts at an event,
such as BeginPlay, a tick or an input action. Coloured **data** wires carry values between
pins. Nodes without exec pins (maths, getting a component's field) are **pure**: they're
worked out whenever a node on an exec wire needs their value.

There are two kinds of circuit:

- An **entity circuit** runs on every entity whose `Circuit` component names it. Each
  entity has its own copy of the circuit's variables.
- The **game circuit** runs once for the whole game, next to the C++ GameMode. The
  project names it with `game_circuit` in its `.swproject`.

Circuits and C++ work together. A C++ GameMode's hooks run first, then the game circuit,
then entity circuits. Game code can add its own nodes with `SW_NODE`.

## Try it

The sample project has two circuits:

- `Content/Circuits/Spinner.swcircuit` turns its entity every tick. Press Space (the
  `pulse` action) to switch between fast and slow.
- `Content/Circuits/Game.swcircuit` is the sample's game circuit. It counts pulses in the
  log.

Run `Scenes/circuits.swscene` in the editor (press Play), or with the player:

```sh
build/bin/player --scene Games/Sample/Scenes/circuits.swscene
```

To edit a circuit as a graph, open Window > Circuits in the editor, or double-click a
`.swcircuit` in Content. The graph editor and a text editor work on the same file, so you
can switch between them. See [Edit circuits in the editor](#edit-circuits-in-the-editor).

## Edit circuits in the editor

The Circuits window (Window > Circuits) shows one circuit as a graph. To open one,
double-click a `.swcircuit` in Content, pick it from the window's list, or press Edit
circuit on an entity's Circuit component. To make a new one, press New, or right-click in
Content and choose New circuit.

| To | Do this |
|---|---|
| Add a node | Right-click empty space, then search. Component nodes appear once you type. |
| Connect pins | Drag from one pin to another. A red wire says why they don't fit. |
| Add a connected node | Drag a wire from a pin and let go on empty space. The new node is connected to it. |
| Give an input a value | Type in the field next to an unconnected input: a value, or a variable's name. |
| Delete | Select nodes or wires and press Delete, or right-click them. |
| Rename a node | Right-click it and choose Rename. Wires to it follow. |
| Copy, paste, duplicate | Ctrl+C, Ctrl+V, Ctrl+D while the graph has focus and no text field is being edited. Internal wires are copied; connections to unselected nodes are omitted. |
| Annotate | Type a note beneath a node title. Notes are editor metadata and do not execute. |
| Arrange nodes | Arrange places nodes in a spaced grid; Undo restores their positions. |
| Organize wires | Insert a typed `Data.Reroute.<Type>` node. |
| Move around | Drag with the right mouse button; scroll to zoom. Fit shows the whole circuit. |
| Undo, redo, save | The buttons, or Ctrl+Z, Ctrl+Y and Ctrl+S while the window has focus. |

The panel on the left lists the circuit's variables, with their types and starting values.
Renaming a variable renames its uses. While you play, it shows each variable's value
now: for the game circuit, or for the selected entity if it runs this circuit.

The circuit compiles with every change. Problems are listed on the left, and the nodes
they're about get a red (error) or yellow (warning) border. A circuit with errors can
still be saved, but it won't run until they're fixed. Saving while playing puts the new
version into the running game.

To run a circuit on an entity, drag the `.swcircuit` from Content onto the entity in the
viewport or the Hierarchy. To make one the game circuit, pick it under Game circuit in
Project settings.

Unconnected number, vector, rotation and colour pins have typed controls. Reflected enum
pins use the component's named options. **Text** opens an explicit literal/variable editor;
Enter commits, including an empty value to restore the default. **Pick** offers entities,
input actions and matching content assets while keeping hand-written values available.

### Debug a running graph

Press Play and select an entity running the open circuit, or open the game circuit.
Enable **Record execution** to keep the latest 256 executed nodes across the session.
Expand an entry for its input/output values as they were at execution, or use **Locate**
to select its node. Recording is optional and avoids formatting pin values when disabled.

Use a node's **Break** checkbox to stop before it executes. Breakpoints apply to that node
in every instance of the circuit. **Step** executes the pending node and stops before the
next node (including pure nodes); **Continue** finishes the interrupted wire. Loop and
sequence stacks and the instruction budget survive suspension. Event entry nodes are
not breakpoint targets. The debugger shows the actual stopped instance, even when another
instance of the same graph hit the breakpoint.

Gameplay ticks and gameplay frame hooks are held while the debugger is stopped; the
editor remains responsive. Audio, UI and host-side visual effects are not a full simulation
snapshot. Other event handlers after the interrupted wire are not replayed for that event.
Use this debugger to inspect one wire, not to establish multiplayer timing or determinism.
Stop destroys the session, including breakpoints and history. Circuit hot reload resumes
when execution continues; it cannot replace the bytecode under a suspended stack.

### Run and inspect from the command line

```sh
build/bin/player --circuit Circuits/Spinner.swcircuit --run-circuit --circuit-ticks 2
build/bin/player --circuit Circuits/Spinner.swcircuit --run-circuit --break-node spin --circuit-steps 1
```

The runner prints the bounded execution history, pin values, variables and paused node.
It runs BeginPlay and the requested fixed/frame ticks on an isolated entity named
CircuitRunner with a Transform, or runs the project's game circuit without an entity.
Tick/step counts must be integers from 0 to 10000. An unknown breakpoint or runtime error
fails the command. Input, physics, audio and UI hosts are absent in this isolated runner;
service nodes requiring them do nothing. Use the actual player for full gameplay tests.

Node notes are readable editor metadata:

```text
editor
  tick -560 0
  @note tick "Fixed simulation tick"
end
```

`--check` rejects annotations naming unknown nodes and invalid enum literals. Notes,
positions and copied graphs still use the same `.swcircuit` format. Pasting preserves
notes and internal wiring, adds missing variables, and renames variables whose existing
name has a different type. Same-name, same-type variables intentionally share the existing
variable. The graph's clipboard is internal to the editor, across open graphs.

Pin colours come from the theme (`circuit_exec`, `circuit_number` and so on; see
[UI](UI.md)).

## Attach a circuit to an entity

Add a `Circuit` component and name the file inside Content:

```text
entity "Spinner"
  Transform
  MeshRenderer
    mesh cube
  Circuit
    circuit "Circuits/Spinner.swcircuit"
  CircuitVars
    values "speed=90"
end
```

`CircuitVars` is optional. It gives this entity its own starting values for the circuit's
variables, written as `name=value; name=value`. While the game plays, circuits write the
variables they change back into it. That's how they show in the Inspector and get into
save games.

Turn `enabled` off on the `Circuit` to stop it.

## The game circuit

Name it in the `.swproject`, with a path relative to the project file, inside Content:

```text
game_circuit "Content/Circuits/Game.swcircuit"
```

The project won't load if the file is missing. The game circuit can use events entity
circuits can't: BeforeSave, AfterLoad and UICommand (a UI element's
`onclick="command"`). There's no entity to run on, so `self` is none.

Its variables are saved with save games automatically, as values named `circuit.<name>`.
They're written after BeforeSave runs and read back before AfterLoad runs.

## The file format

A circuit is plain text. People, diffs and AI tools can all read it:

```text
shockwave_circuit 1
# Turns its entity around the up axis at speed degrees a second.

var speed Float 45

node tick Event.FixedTick

node spin Component.Transform.Set.rotation
  exec tick.then
  in rotation = turned.result

node turned Rotation.Combine
  in first = now.rotation
  in second = step.result

node now Component.Transform.Get.rotation

node step Rotation.AroundY
  in degrees = angle.result

node angle Math.Multiply
  in a = tick.dt
  in b = speed

editor
  tick -560 0
  spin 140 0
end
```

Line by line:

| Line | Meaning |
|---|---|
| `shockwave_circuit 1` | The format version. Always the first line. |
| `# ...` | A comment. Comments before the first `var` or `node` describe the circuit. Comments take a whole line. |
| `var <name> <Type> [value]` | A variable, with its starting value. Without one it starts at zero, false or empty. |
| `node <name> <Type>` | A node. The name is yours, made of letters, digits and `_`. The type is from [the node list](CircuitNodes.md). |
| `exec <node>.<output>` | Under a node: it runs after that exec output. |
| `exec <input> = <node>.<output>` | The same, into a named exec input, for nodes with several (Flow.Gate's `open`, say). |
| `in <pin> = <value>` | Under a node: what goes into an input. Pins you leave out use their default. |
| `editor` ... `end` | Where each node sits in the graph editor. It comes last, so moving nodes doesn't change the lines above. |

An input takes one of three things:

- a node's output, `node.pin`, such as `tick.dt`;
- a variable, by name, such as `speed`;
- a literal value.

Literals depend on the pin's type:

| Type | Literal | Notes |
|---|---|---|
| `Bool` | `true`, `false` | |
| `Int` | `3` | An Int also goes into a Float pin. |
| `Float` | `2.5` | |
| `Vec2`, `Vec3` | `1 2`, `1 2 3` | |
| `Color` | `1 0.5 0 1` | Alpha is optional. |
| `Rotation` | `0 90 0` | Euler angles in degrees, as scenes write them. |
| `String` | `"text"` | Quoted. |
| `Entity` | `self`, `none`, `"Door"` | A quoted name is looked up when the node runs. Entity inputs default to `self`. |
| `Mesh`, `Texture`, `Model` | `"cube"` | An asset's name. |

An exec output leads to one node. To do several things in order, use Flow.Sequence. An
exec input can be reached from many outputs.

## Events

| Event | When | Outputs | Circuits |
|---|---|---|---|
| `Event.BeginPlay` | Play starts. For an entity that appears later, when it first runs. | | both |
| `Event.EndPlay` | Play stops. | | both |
| `Event.FixedTick` | Every simulation tick, after the GameMode's `fixedUpdate`. | `dt` | both |
| `Event.Frame` | Every rendered frame, after the GameMode's `frame`. | `dt` | both |
| `Event.ActionPressed` | An input action was pressed this frame. Its `action` input names it. | | both |
| `Event.ActionReleased` | An input action was released this frame. | | both |
| `Event.TriggerEnter` | Something started touching a sensor collider. Both entities hear about it. | `other` | entity |
| `Event.TriggerExit` | Something stopped touching a sensor collider. | `other` | entity |
| `Event.BeforeSave` | A save game is about to be written. | | game |
| `Event.AfterLoad` | A save game was loaded. | | game |
| `Event.UICommand` | A UI element's `onclick="command"` ran. Its `command` input names it. | `value`, `element`, `document` | game |

An `action` or `command` input must be a literal: `in action = "jump"`.

## Nodes

[CircuitNodes.md](CircuitNodes.md) lists every node with its pins. Regenerate it after
adding nodes or components:

```sh
build/bin/player --node-schema docs/CircuitNodes.md
```

The main groups:

- **Flow:** Branch, Sequence, DoOnce, Gate, FlipFlop, ForLoop.
- **Math, Logic, Vector, Rotation, Convert, Text:** values.
- **Entity:** Self, FindByName, IsValid, Spawn, SpawnAt, Destroy.
- **Input, Audio, UI, Physics:** the game's services.
- **Debug.Print:** writes to the log and the editor's Console.
- **Component:** made for every registered component, your game's own included.
  - `Component.<Name>.Get` gives every field and `found`.
  - `Component.<Name>.Set` sets the fields you give it and leaves the rest alone.
  - `Component.<Name>.Has`, `.Add` and `.Remove` do what they say.
  - `Component.<Name>.Get.<field>` and `Component.<Name>.Set.<field>` work on one field.
- **Variables:** every variable has `Get.<name>` (output `value`) and `Set.<name>` (exec,
  input `value`). You can also use a variable directly as an input: `in b = speed`.

## Add nodes from C++

In your game module, turn a function into a node with `SW_NODE`:

```cpp
#include <Circuits/CircuitNodes.h>

SW_NODE("Combat", "ApplyDamage", [](shockwave::CircuitCall& call, shockwave::Entity target, float amount) {
    if (auto* health = call.world.get<Health>(target)) health->current -= amount;
}, "target", "amount")

SW_PURE_NODE("Combat", "IsDead", [](float health) { return health <= 0.0f; }, "health")
```

This makes `Combat.ApplyDamage`, with exec pins and inputs `target` and `amount`, and
`Combat.IsDead`, a pure node with output `result`:

- **Pins** come from the parameters. They can be `bool`, `int`, `float`, `glm::vec2`,
  `vec3`, `vec4` (Color), `glm::quat` (Rotation), `std::string` or `Entity`.
- **A leading `CircuitCall&`** gives the node the world, `self` and the services.
- **A return value** becomes the output `result`.

Nodes register when the game module loads and go away with it. Reloading game code
compiles running circuits again.

## Check circuits

`player --check` compiles every `.swcircuit` in Content, the game circuit as one. It
reports each problem as `file:line`:

- an unknown node type, pin, variable or node name;
- a type mismatch, for example a Float output into a String input;
- a required input with nothing in it;
- an exec output that leads to two nodes;
- a loop of data wires, where a value would depend on itself;
- an event the circuit's kind can't use, such as UICommand in an entity circuit;
- a scene's `Circuit` that names a missing file, or `CircuitVars` with a variable the
  circuit doesn't have, or the wrong type of value.

Nodes that never run, and pure nodes nobody reads, are warnings: they're printed but
don't fail the check.

## See what a circuit compiles to

Loading compiles a circuit to bytecode. Every entity running it shares the bytecode and
keeps only its own registers. To print the bytecode:

```sh
build/bin/player --circuit Circuits/Spinner.swcircuit --dump
```

Each node on an exec wire is a block: `eval` lines for the pure nodes it needs, in order,
then one `fire` line. The `fire` line runs the node and jumps to the block its chosen exec
output leads to.

## Edit while playing

Saving a `.swcircuit` while the game plays compiles it again within half a second.
Running circuits switch to the new version and keep variables whose name and type
haven't changed. A version with errors is reported and the old one keeps running.

## Limits

- There are no standalone comment groups, graph functions, or native-clipboard interchange yet.
- Debug stepping follows the current wire; remaining handlers of the interrupted event
  are not queued for later replay.
- Circuits run on the main thread, one event at a time. A parallel mode that runs entity
  circuits across cores is planned. The compiler already records which components each
  circuit reads and writes, so the scheduler will be able to run them safely.
- There are no latent nodes yet (Delay, MoveTo, wait for an animation).
- One event can run at most a million steps. A circuit that loops forever is stopped and
  reported, and the game carries on.
- Pure nodes are worked out again each time a node on an exec wire needs them. When
  Sequence or ForLoop goes round again, the nodes feeding them are not worked out again.

## Gameplay commands and component methods

Build behavior from an event, an action, and a result. Right-click the graph to browse
Events and Gameplay actions first. Queries, variables, math and reflected component
access have their own collapsible sections; search includes descriptions.

The sidebar lists public commands with buttons and editable Amount/Value payloads.
During Play, select an entity running the open graph to invoke its commands; game
circuit commands target the game instance.

A circuit can expose named commands with `Event.Message`. Its required `message` input
is a nonempty literal. Outputs are `sender` (Entity), `amount` (Float), and `value`
(String): a fixed typed command envelope. `Circuit.Send` addresses one target and supplies
that envelope. Its `accepted` result means queued, not completed. An absent handler,
stale entity or full queue returns false. Target `none` addresses the game circuit.
Use commands such as Open, Close, Activate and ApplyDamage instead of reaching into
another entity's private variables.

`Content/Circuits/Door.swcircuit` is a four-node example: Open raises a door and Close
lowers it. Verify it without the editor:

```sh
build/bin/player --circuit Circuits/Door.swcircuit --run-circuit --send-message Open
```

The isolated runner supplies a Transform; its trace shows the invoked action and pins.
An unknown message fails with exit code 1. `player --check` rejects missing or empty
message names and incorrectly typed pins. Dynamic target/handler existence is checked
at runtime, because entities may spawn or disappear.

C++ game code can expose a component's member method directly:

```cpp
struct Health {
    float current = 100;
    float damage(float amount) { current -= amount; return current; }
};
// Register Health as a component using the normal game-module mechanism.
SW_METHOD("Combat", "Damage", &Health::damage, "amount")
```

This creates `Combat.Damage` with exec/then, a target Entity defaulting to self, a Float
amount, and a Float result. Void methods omit the result. Missing required components
produce a VM diagnostic. Methods currently support non-const member functions and the
same parameter types as SW_NODE; disambiguate overloaded functions with an explicit
member-pointer cast. SW_NODE remains the entry point for free functions and service
adapters. Registrations follow the existing module hot-reload lifecycle.

From main-thread C++ session code, use `CircuitWorld::send(target, "Open", sender,
amount, value)`. Commands are FIFO and delivered at the next frame or fixed-tick
boundary. Handler-generated messages wait for a subsequent boundary, preventing
recursive stacks and infinite same-frame dispatch. The pending queue is capped at 1024;
overflow is rejected and diagnosed. Stop cancels pending commands. Messages are transient,
not saved, and this API is not a cross-thread mailbox. Breakpoints retain the existing
single-wire stepping limitation; unrun handlers of an interrupted message are not replayed.

`Transform.SetPosition`, `Transform.Rotate`, and `Transform.RotateByRate` are direct gameplay actions with a
success result. They modify local transforms immediately, not physics bodies; use the
physics actions for dynamic bodies. Commands do not yet have arbitrary user-declared
parameter lists, return values, latent completion, or graph-local function calls.

The Spinner sample uses FixedTick → RotateByRate directly. Speed and dt are inputs;
the quaternion composition stays inside the action rather than filling the graph.
