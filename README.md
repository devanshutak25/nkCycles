Cycles for Houdini 22
=====================

Blender's [Cycles](https://www.cycles-renderer.org) path tracer as a **Hydra render delegate for Houdini 22
(Solaris and husk)**, with NVIDIA GPU rendering (OptiX / CUDA), denoising, Cycles shader nodes laid out like
Blender, and Blender-style render settings.

This is a fork of [cjhosken/hdCycles](https://projects.blender.org/cjhosken/hdCycles) (itself based on Blender's
Cycles repository). The work for Houdini 22 lives on the `houdini-22` branch.

> **Status: work in progress.** Daily lookdev with Cycles shaders and USD lights works. Volumes, most AOVs,
> implicit USD shapes and Karma/MaterialX materials do **not** work yet; see [Known issues](#known-issues).

## Tested with

| | |
|---|---|
| Houdini | 22.0.429 (USD 26.05), Windows 11 |
| GPU | NVIDIA RTX 4090, OptiX 8.0 SDK, CUDA |
| Other plugins | Loads alongside Arnold (HtoA): Embree is linked statically without SYCL |

Linux and macOS builds have not been tested on Houdini 22.

## What works

| Area | State |
|---|---|
| Rendering in the Solaris viewport and with husk | Works |
| GPU rendering | OptiX and CUDA, chosen automatically (OptiX > CUDA > CPU), results match the CPU |
| Denoising | OpenImageDenoise (GPU or CPU) and OptiX |
| Shaders | All 96 user-facing Cycles shader nodes, as Houdini VOP nodes inside a **USD Cycles Material Builder** |
| Shader node UI | Blender 5.2 order, labels, menus, collapsible panels, hide rules and default values |
| USD lights | Sphere, rect, disk, distant and dome lights match Karma within 1% (normalized and not) |
| Render settings | **Cycles Render Settings LOP**, a Cycles tab on the stock Render Settings LOP, and viewport display options, all laid out like Blender's render properties |
| Interactive edits | Moving/editing lights, objects, materials and settings updates the render |
| Viewport | Free view and looking through a camera |
| Stability | No crash on quit, renderer switch or device change |
| Units | Stages with `metersPerUnit` 1 and 0.01 |

## Known issues

### Rendering
- **Volumes don't render.**
- **Only 5 AOVs work:** color, depth, normal, primId, instanceId. Light passes (diffuse/glossy/transmission,
  emission, environment, AO, shadow), data passes (position, UV, vector, mist, ...), cryptomatte, shader AOVs
  and light path expressions are missing, and there is no AOV UI yet.
- **Implicit USD shapes don't render** (Sphere, Cube, Cone, Cylinder, Capsule). Convert them to meshes for now.
- **Karma / MaterialX materials are not translated.** Use the Cycles Material Builder.
- **OSL** is not available in Houdini builds.
- The Principled Hair BSDF needs a recheck of OptiX vs CPU output.

### Viewport
- The camera sometimes jumps very far from the origin while orbiting. Likely cause: the `depth` AOV carries
  Cycles' camera distance instead of the normalized depth Houdini expects.

### UI
- Port locations are wrong on some shader nodes.
- The Cycles entries in the Tab menu are not grouped into sub-menus like Blender's Add menu.
- The Cycles Render Settings LOP needs UX touch-ups.
- Some shader node defaults are raw Cycles defaults (e.g. Checker is black on black).
- Color Ramp and the curve nodes expose raw arrays instead of Houdini ramp parameters; angles are in radians.

### Missing features
- Per-object and per-light settings (visibility, shadow catcher, holdout, ...).
- Motion blur, sample subset and the Fast GI method menu are not exposed.
- Viewport resolution divider (the viewport always renders at full resolution).
- HIP (AMD) and oneAPI (Intel) devices are disabled; NVIDIA and CPU only.

## Roadmap

Roughly in this order:

1. Fix the orbit jump (depth AOV).
2. Volumes.
3. Shader node ports, Tab menu sub-menus, Render Settings LOP touch-ups, node defaults, ramps and degrees.
4. AOVs: Cycles passes, cryptomatte, shader AOVs, an AOV UI, multi-AOV EXR output from husk.
5. Implicit USD shapes.
6. Per-object / per-light settings.
7. MaterialX / Karma material translation.
8. OSL.

## Building

Full instructions: [BUILDING.md](BUILDING.md), section *Hydra Render Delegate for Houdini*. In short, on Windows
with Visual Studio 2022, CMake, Ninja, the CUDA Toolkit and the OptiX 8 SDK headers:

```
.\make.bat update
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DHOUDINI_ROOT="C:/Program Files/Side Effects Software/Houdini 22.0.429" ^
  -DWITH_CYCLES_HYDRA_RENDER_DELEGATE=ON ^
  -DWITH_CYCLES_DEVICE_CUDA=ON -DWITH_CYCLES_DEVICE_OPTIX=ON ^
  -DWITH_CYCLES_DEVICE_HIP=OFF -DWITH_CYCLES_DEVICE_ONEAPI=OFF ^
  -DWITH_CYCLES_STANDALONE_GUI=OFF ^
  -DWITH_CYCLES_CUDA_BINARIES=ON ^
  -DOPTIX_ROOT_DIR="<path to OptiX SDK>" ^
  -DCYCLES_CUDA_BINARIES_ARCH="sm_75;sm_86;sm_89;sm_120;compute_75" ^
  -DEMBREE_ROOT_DIR="<path to static Embree without SYCL>"
cmake --build build
cmake --install build
```

- Build Embree 4 as a static library without SYCL (`EMBREE_STATIC_LIB=ON`, `EMBREE_SYCL_SUPPORT=OFF`). The
  precompiled Embree needs the SYCL runtime, which conflicts with other plugins such as Arnold.
- The install step runs Houdini's `hython` to generate the shader nodes and render settings UI, so it needs a
  Houdini license.

## Using it in Houdini

1. Copy `install/houdini/packages/cycles.json` into `Documents/houdini22.0/packages` (adjust the path inside it if
   you move the install), or start Houdini with `PXR_PLUGINPATH_NAME=<install>/houdini/dso/usd_plugins`.
2. In Solaris, pick **Cycles** in the viewport's renderer menu.
3. Materials: in a Material Library LOP, create a **USD Cycles Material Builder** and build the shader from the
   Cycles nodes inside it.
4. Settings: use the **Cycles Render Settings** LOP (or the Cycles tab of the Render Settings LOP). Viewport-only
   settings are in the viewport's display options.
5. Batch rendering: `husk -R Cycles scene.usd`.

**Device:** `AUTO` by default (best GPU, then CPU). Override with the `cycles:device` render setting
(`AUTO`, `OPTIX`, `CUDA`, `CPU`) or the `CYCLES_DEVICE` environment variable, e.g.
`CYCLES_DEVICE=CPU husk -R Cycles scene.usd`.

## Changes in this fork

Commits on `houdini-22` on top of upstream `main`, among them:

- Build against Houdini 22 / USD 26.05 (missing `hdsi` link, packaging), static Embree without SYCL.
- GPU by default (`AUTO` device), OptiX/CUDA build options, denoising on the render device.
- Render settings UI: Cycles tab, viewport options and the Cycles Render Settings LOP, Blender layouts and defaults.
- Shader nodes as Houdini HDAs with Blender layouts (`src/hydra/houdini/blender_node_layouts.json`, generated by
  `src/hydra/houdini/src/blender_node_layouts.py` running inside Blender) and Blender defaults applied by the
  delegate (`shader_defaults.json`).
- Fixes: crash on delegate teardown, light transform updates, frozen/squashed image when looking through a camera.

See `git log main..houdini-22` for the full list.

## License and credits

Cycles is licensed under the Apache License 2.0, see [LICENSE](LICENSE). Cycles is developed by the Blender
Foundation and contributors ([AUTHORS](AUTHORS)); the Houdini Hydra integration builds on the work of
[cjhosken/hdCycles](https://projects.blender.org/cjhosken/hdCycles). Houdini is a product of SideFX; this project
is not affiliated with SideFX or the Blender Foundation.
