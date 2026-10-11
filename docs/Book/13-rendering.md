# 13. Drawing the picture: the renderer

**In this chapter:** lights and the Environment, how a frame is drawn on the GPU, and
why the renderer is a plugin.

## Lighting a scene

Lights are entities too. A `Light` has a `type`:

| Type | Like | Position matters? | Direction matters? |
|---|---|---|---|
| `Directional` | the sun: parallel rays from far away | no | yes |
| `Point` | a bare bulb: light in every direction, fading out at `range` | yes | no |
| `Spot` | a torch: a cone of `spot_angle` degrees | yes | yes |

A light shines along its Transform's -Z axis, so rotating the entity aims it. All three
cast soft shadows. A scene with no lights at all gets a default sun, so you're never
staring at black.

The **Environment** entity sets the mood: the sky color, the ground color, how much
ambient light fills the shadows, the **exposure** (overall brightness, in camera
stops) and the **tonemapper**.

Try making Orb Hunt a night scene. Dim the sun and add a point light that follows the
player, as a child of the Player:

```text
entity "Player light"
  Transform
    position 0 2 0
  Parent
    entity "Player"
  Light
    type Point
    color 1 0.7 0.4 1
    intensity 8
    range 7
end
```

## HDR and tonemapping, simply

Real light covers a huge range: a lamp is hundreds of times dimmer than the sun. A
screen can only show brightness from 0 to 1. So Shockwave draws the scene in **HDR**
(high dynamic range), where a color can be 5 or 50, and then a **tonemapper** squeezes
that into what the screen can show, the way a camera does. ACES (the default) and AgX
give a filmic look; Reinhard is softer; None clips.

That's why the orbs' `emissive_intensity 2` makes them glow brighter than "white".

## How one frame is drawn

Each frame, the game hands the renderer a camera, a list of meshes and a list of lights.
The renderer turns that into GPU work, as a series of **passes**. Each pass reads some
images and writes others:

```text
skinning ─▶ shadows (sun) ─▶ shadows (lights) ─▶ scene ─▶ particles ─▶ tonemap ─▶ ui ─▶ gui ─▶ present
                                                 (HDR)                  (to screen      (editor
                                                                         colors)         panels)
```

| Pass | What it does |
|---|---|
| skinning | bends animated characters into this frame's pose (only if there are any) |
| shadows | draws the scene from each light's point of view, to know what's in shadow |
| scene | draws every mesh with lighting and shadows, into an HDR image |
| particles | blends fire, smoke and sparks into the HDR image |
| tonemap | turns HDR colors into screen colors |
| ui | draws the game's HUD and menus, sharp, at full resolution |
| gui | draws the editor's panels (in the player, the image is copied to the window instead) |
| present | shows the finished image |

These passes are arranged in a **render graph**. Each pass declares which images it
uses and how. From that, the graph works out the *barriers*: the points where the GPU
must finish writing an image before the next pass reads it. Getting barriers right by
hand is one of the hardest parts of Vulkan; the graph does it every frame. **Window >
Stats** lists the passes and barriers of the current frame.

In the editor, the tonemapped image isn't put straight on screen. It's shown *inside*
the Viewport panel, as a picture in the UI, and the panels are drawn around it.

## The renderer is a plugin

Games and the editor never talk to Vulkan. They talk to the `Renderer` class (the
`Renderer` module), which has simple calls like `setCamera`, `drawMesh` and
`addLight`. Behind it sits a **render backend**, loaded from a plugin file while the
program runs:

| Plugin | What it does |
|---|---|
| `renderer_vulkan` | the real renderer, with everything above |
| `renderer_null` | draws nothing; used by `player --check` and servers, so they need no GPU |

Because the Renderer keeps the meshes, textures and settings, it can swap the backend
between two frames without the game noticing. That's how you can rebuild the Vulkan
renderer, or just a shader, and reload it while the editor runs (chapter 14).

> **Under the hood.** The Vulkan renderer lives in `Source/Plugins/RendererVulkan`. Its
> frame is built in `RendererFrame.cpp`; shadows, particles, skinning, game UI and the
> editor GUI each have their own file. The low-level pieces (device, swapchain,
> pipelines, the render graph itself) are the `RHI` module, which only this plugin
> links. Shaders are in `Shaders/` and are compiled to SPIR-V by the build. Gathering
> draws (`renderWorld` in `Engine/Systems.h`) runs across the job system, and
> `drawMesh` is safe from any thread, so thousands of objects are collected quickly.

## Recap

- Lights are entities: Directional, Point or Spot, aimed along -Z. The Environment sets
  sky, ambient, exposure and the tonemapper.
- The scene is drawn in HDR, then tonemapped for the screen.
- A frame is a render graph of passes; the graph places the GPU barriers.
- The renderer is a swappable plugin behind the `Renderer` class.

Next: [Changing code while it runs](14-hot-reload.md).
