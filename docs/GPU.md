# Choosing the GPU

Many PCs have more than one GPU: a graphics card, the GPU built into the CPU, and
sometimes a software one that runs on the CPU. Shockwave picks one when it starts, tells
you which and why, and warns you when the choice will be slow or fragile. You can also
pick one yourself.

## See which GPU Shockwave uses

Run this from the repository folder:

```sh
./build/bin/player --gpus
```

It lists every GPU Vulkan can see, marks the one in use with `*`, and says why it was
picked. On a desktop with an RTX 3080 and an Intel iGPU it looks something like this:

```text
GPUs Vulkan can see:
  0  Intel(R) UHD Graphics 770 (ADL-S GT1)
     integrated, Intel, Vulkan 1.4.318, driver Intel open-source Mesa driver Mesa 26.2.4, PCI 0000:00:02.0
* 1  NVIDIA GeForce RTX 3080
     dedicated, NVIDIA, Vulkan 1.4.312, driver NVIDIA 580.95.05, 10240 MB, PCI 0000:01:00.0
     your screen is plugged into it
  2  llvmpipe (LLVM 20.1.8, 256 bits)
     software (runs on the CPU), Mesa, Vulkan 1.4.318, driver llvmpipe Mesa 26.2.4
Screens are plugged into: NVIDIA GPU (card1, PCI 0000:01:00.0) DP-1
Using 1 NVIDIA GeForce RTX 3080. It's the fastest kind available (dedicated).
```

Any problem shows as a `Warning:` line at the end. Add `--json gpus.json` to also write
the report as JSON, for scripts and AI tools. The `warnings` list is empty when all is
well.

The same report is in two other places:

- **Every log.** The player and the editor print it when the renderer starts. Warnings
  also appear in the editor's Console.
- **The editor's Stats window** (Window > Stats). Warnings show in yellow under the GPU
  name, and the **GPUs** section lists every GPU. Hover one for its details.

## Pick a GPU yourself

Name the GPU by its number from the list, or by any part of its name. Case doesn't
matter.

| Where | How |
|---|---|
| Editor | Preferences > Plugins > GPU. The renderer restarts on the new GPU straight away, and your scene stays loaded. "Automatic" goes back to Shockwave's choice. |
| Editor, one run | `./build/bin/editor --gpu nvidia` |
| Player | `./build/bin/player --gpu 3080` |
| Any program | `SHOCKWAVE_GPU=intel ./build/bin/player` |

The editor saves its choice as `gpu = ...` in its preferences file. The `--gpu` option
wins over `SHOCKWAVE_GPU`, which wins over Preferences.

If the GPU you name doesn't exist or can't run Shockwave, you get a warning and the
automatic choice instead.

## How the automatic choice works

1. Only GPUs that can run Shockwave count. They need Vulkan 1.3, the features the
   renderer uses, and a way to draw into the window. `player --gpus` says what each other
   GPU is missing.
2. The fastest kind wins: a dedicated card, then an integrated GPU, then a virtual one,
   then a software one.
3. Between two of the same kind, the one your screen is plugged into wins, then the one
   with more memory.

## The warnings, and what to do

**"... is rendering, but your screen is plugged into ..."** Each frame is drawn on one
GPU and copied to the other for display. That adds latency, and some drivers can't do it
at all, so the swapchain fails to start. Run with `--gpu` and the GPU named in the
warning. On a laptop with switchable graphics this setup is normal and fine.

**"Your screen is plugged into an NVIDIA GPU ... that Vulkan can't see"** That GPU's
Vulkan driver isn't loading. The most common cause is updating the driver without
rebooting, so the kernel module and the user-space driver no longer match. Reboot first.
If it still happens, check that `vulkaninfo --summary` lists the GPU, and install its
driver. On Arch-based systems:

```sh
sudo pacman -S nvidia-utils       # NVIDIA
sudo pacman -S vulkan-radeon      # AMD
sudo pacman -S vulkan-intel       # Intel
```

**"... renders on the CPU"** Only the software GPU works, so frame rates will be very
low. The warning says why each real GPU can't be used.

**"... is faster but can't run Shockwave"** A better GPU is there but lacks something,
usually a driver too old for Vulkan 1.3. Update its driver.

## How it works

The Vulkan renderer lists every GPU and checks each against the renderer's needs
(`Source/Runtime/RHI/Private/Context.cpp`). To learn which GPU the screen is plugged into,
it reads the connected screens from `/sys/class/drm` on Linux. It then matches them with
each GPU's device number (`VK_EXT_physical_device_drm`) or PCI address
(`VK_EXT_pci_bus_info`).

The decision itself lives in `Source/Runtime/Renderer/Private/GpuSelection.cpp` and
doesn't touch Vulkan, so `Tests/GpuSelectionTest.cpp` checks it on made-up machines. On
Windows and macOS the screen check isn't done yet, so the screen warnings don't appear
there.
