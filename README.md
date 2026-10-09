# Shockwave

A modern 3D game engine in C++20, rendering natively with Vulkan. Open source (MIT),
no royalties. Linux first, with Windows and macOS (through MoltenVK) as later ports.

## Design principles

- **Multithreaded from the start.** A work-stealing job system runs on every core.
  Engine APIs say whether they are safe to call from worker threads, and the hot ones are
  (mesh creation, draw submission).
- **Modern GPU techniques only, no legacy paths.** Vulkan 1.3 with dynamic rendering,
  synchronization2, timeline semaphores, buffer device address with vertex pulling,
  a GPU draw table indexed per draw, bindless textures, and reverse-Z depth with an
  infinite far plane.
- **Frame-rate independent.** Simulation ticks at a fixed rate (60 Hz by default) and
  rendering interpolates between ticks, so gameplay is identical at 30 or 500 fps.
  The frame rate is uncapped by default (mailbox present, no tearing).
- **Resolution and aspect independent.** The scene renders into its own HDR target at a
  configurable render scale, then is scaled to whatever size and shape the window is.
- **Distro agnostic.** Every library is fetched by CMake; Vulkan and the window system
  are found at runtime.

## Stack

- **C++20 + CMake**
- **SDL3**: windows, input, audio and timing on every platform
- **Vulkan 1.3**, loaded at runtime by **volk**; setup through **vk-bootstrap**
- **VMA** for GPU memory, **GLM** for math
- **Dear ImGui** (docking branch) for the editor and in-game debug UI, **ImGuizmo** for gizmos
- **GLSL** shaders compiled to SPIR-V at build time

## Layout

```
Shockwave/
  CMakeLists.txt        top-level build; fetches dependencies
  cmake/                build helpers (shader compilation, Dear ImGui)
  engine/               the engine, built as a static library (shockwave)
    include/shockwave/    public API games include (no Vulkan types in here)
    shaders/              engine shaders (GLSL)
    src/core/             job system
    src/input/            keyboard, mouse and action mapping
    src/render/           camera, built-in meshes
    src/ui/               Dear ImGui theme and helpers
    src/vulkan/           Vulkan backend: device, swapchain, pipelines, memory
  editor/               Shockwave Editor (Dear ImGui), built on the engine
  sandbox/              a small test scene that uses the engine
  scenes/               scene files (.swscene, plain text)
```

## Build and run

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/bin/sandbox
```

The window title shows frame rate, frame time, render resolution and draw count.

Controls: hold the right mouse button to look, WASD to move, E/Q (or Space/Ctrl) up and
down, Shift to go faster, scroll to change speed, Esc to quit.

Sandbox options: `--grid 100` sets the cube grid (100 = 10,000 cubes), `--scale 0.5`
renders at half resolution, `--vsync` caps to the display, `--gui` shows a Dear ImGui
debug overlay, `--frames 300` quits after 300 frames.

### Editor

```
./build/bin/editor                       # opens scenes/main.swscene
./build/bin/editor path/to/level.swscene
```

A docked layout around a live viewport: Hierarchy (entities), Inspector (transform, mesh,
texture, tint), Renderer (frame time graph, render scale, vsync, sky color, camera) and Log.
Panels can be dragged, docked and tabbed anywhere; View > Reset layout restores the default.
The layout is remembered in `~/.local/share/Shockwave/Shockwave Editor/`.

Editor controls (Help > Controls, or F1, lists them too):

- Hold the right mouse button in the viewport to fly: mouse to look, WASD to move,
  E/Q up and down, Shift faster, mouse wheel changes speed
- Mouse wheel dollies, middle mouse drag pans
- Left click selects; W / E / R switch the gizmo to move / rotate / scale; T toggles
  world or local axes; hold Ctrl while dragging to snap
- F (or double-click in the hierarchy) focuses the selection; Ctrl+D duplicates, Del deletes
- Ctrl+Z / Ctrl+Y undo and redo; Ctrl+N / Ctrl+O / Ctrl+S new, open, save

Scenes are plain text, one property per line, so they diff and merge cleanly in git.
The editor runs with vsync on (an editor mostly sits idle); untick VSync in the Renderer
panel or start it with `--uncapped` to render flat out.

Debug builds turn on the Vulkan validation layers if they are installed and print
any API misuse to the terminal. For a stricter check, run with
`VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1` to enable synchronization validation too.

### System packages

CachyOS / Arch (the main development platform):

```
sudo pacman -S --needed base-devel cmake ninja git glslang \
    vulkan-icd-loader vulkan-validation-layers vulkan-tools \
    libx11 libxext libxrandr libxcursor libxi libxtst libxss libxkbcommon \
    wayland wayland-protocols libdecor
```

Plus the Vulkan driver for your GPU: `vulkan-radeon` (AMD), `vulkan-intel` (Intel)
or `nvidia-utils` (NVIDIA). CachyOS usually installs this already; `vulkaninfo --summary` shows it.

`glslang` is optional: without it the build compiles glslang from source.
The editor's Open and Save As dialogs use `xdg-desktop-portal` (installed with KDE and
GNOME) or fall back to `zenity`.
Other distros need the same things under their own names; SDL keeps a list for each:
https://wiki.libsdl.org/SDL3/README-linux#build-dependencies

## Milestones

1. Window, Vulkan device and swapchain, fixed-timestep game loop (done)
2. Job system, mesh upload, bindless vertex pulling, HDR render target with render scale,
   uncapped interpolated rendering (done)
3. 3D basics: reverse-Z depth, back-face culling, fly camera, keyboard and mouse input with
   action mapping, bindless textures with GPU mipmaps, GPU draw table (done)
4. Gamepads, and GPU-driven rendering: frustum culling and indirect draws generated on the GPU
5. Scene: entities and components, with systems scheduled on the job system
6. Render graph, tonemapping and upscaling, lighting, glTF assets, audio
7. Hot reload of game code and shaders
8. Editor (first version done): Dear ImGui docking UI, viewport rendered by the engine at
   panel size, hierarchy, inspector, move/rotate/scale gizmos, click to select, undo/redo,
   plain-text scenes. Next: play mode, asset browser, editing engine entities directly
9. Windows and macOS ports
