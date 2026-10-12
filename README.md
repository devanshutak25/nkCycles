Cycles for Houdini 22
=====================

Blender's [Cycles](https://www.cycles-renderer.org) path tracer as a **Hydra render delegate for Houdini 22
(Solaris and husk)**, with NVIDIA GPU rendering (OptiX / CUDA), denoising, Cycles shader nodes laid out like
Blender, and Blender-style render settings.

This is a fork of [cjhosken/hdCycles](https://projects.blender.org/cjhosken/hdCycles) (itself based on Blender's
Cycles repository). The work for Houdini 22 lives on the `houdini-22` branch.

> **Status: work in progress.** Daily lookdev with Cycles shaders, USD lights and VDB volumes works. Most AOVs,
> native Houdini volumes, implicit USD shapes and Karma/MaterialX materials do **not** work yet; see
> [Known issues](#known-issues).

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
| Volumes | VDB files and VDB volumes made in SOPs inside a LOP stage; volumes without a material show their density |

## Known issues

### Rendering
- **Native Houdini volumes** (not VDB) don't render yet; convert them with a Convert VDB SOP.
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
2. Native Houdini volumes.
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

## Changes from the source repository

Compared with `main` of [cjhosken/hdCycles](https://projects.blender.org/cjhosken/hdCycles), this fork adds the
commits below (about 16,700 lines, most of them the generated `blender_node_layouts.json`). The Houdini shader
node generator is ported from that repository's `shaders` branch, by Christopher Hosken. Everything else is new.

### Build and packaging for Houdini 22

| Change | Why | Commit |
|---|---|---|
| Link `hdsi` when building against Houdini | Houdini 22's USD 26.05 libraries leave it out, so linking failed | `ca48d2cb4` |
| Use Houdini's OpenImageDenoise | Houdini loads its own OIDN 2.3 with the same file name as the precompiled 2.5 | `d7c53a3c0` |
| Fix MaterialXCore not being linked | The CMake block used an undefined variable | `d04a1e39e` |
| Fix the plugin resource path | It was one directory off, so the CUDA/OptiX kernels in `lib/` were not found | `5fae6c8aa` |
| Register the delegate in `UsdRenderers.json` | Without it Cycles is missing from the Solaris renderer menu and `husk -R Cycles` | `301887910` |
| Allow a static Embree without SYCL (`EMBREE_ROOT_DIR`), don't ship the SYCL runtime | The precompiled Embree needs `sycl8.dll`, which clashes with other plugins (Arnold) and stops Cycles loading | `4edfd12f9` |
| Generate shader nodes, render settings UI and the Cycles Render Settings LOP at install time with `hython` | New Houdini install step (`src/hydra/houdini/CMakeLists.txt`) | `3653a9084`, `16f63cd20`, `8abcbdb5f` |

### Rendering fixes

| Change | Commit |
|---|---|
| **Lights match Karma.** Dome lights were π times too bright and distant lights 4 times. Sphere light radius ignored stage units. Normalized lights used the area in meters instead of stage units (10000x too bright on a centimeter stage). | `49383cb65` |
| **Material sockets were not found** for Houdini shader nodes. Sockets now match by identifier or UI name, ignoring case, spaces, underscores and dots. Materials ending in an Emission or Volume output used to fall back to the default shader. | `fe9d20c86` |
| **Uninitialized values** (core Cycles): curves and ramps without points, and Set Normal without a direction, read garbage that differed between CPU and GPU. They now use Blender's defaults. | `64e207902` |
| **Crash on teardown** when quitting Houdini, switching renderer or changing device: OpenGL objects were deleted without a context. A fence leaked on every display update. | `576f64f74` |
| **Moving or rotating a light** did not update the render (the object was not tagged for update). | `379ba058f` |
| **Frozen, squashed viewport image when looking through a camera.** Houdini binds buffers the size of the viewer but sets the viewport to the camera gate. The delegate now renders at the buffer size and conforms the camera aspect. | `ac7c0f2fc` |
| **Volumes without a material rendered nothing** (they got the default surface shader). They now get Cycles' default volume shader, which shows the density like Karma. | `2b69b0a9a` |
| **Volumes made in SOPs failed to load** ("could not open file op:/stage/..."). Houdini passes these as `op:` paths, not files. The delegate now gets the grids from Houdini through `USD_SopVol` (`SOPgetVDBVolumePrimitive`), sharing them instead of copying. This needs the Houdini SDK headers and `libGEO`/`libUT`/`libCE`, so it is only built for Houdini (`houdini_volume.cpp`). | `0c125583d` |

### Render delegate features

| Change | Commit |
|---|---|
| `AUTO` device, the new default: OptiX, then CUDA, HIP, Metal, oneAPI, then CPU. Before, the default was CPU, which made the viewport slow. The device setting also accepts tokens, the form Houdini writes menu values in. | `a4e3c43b6` |
| Film and background render settings (`cycles:film:*`, `cycles:background:*`: exposure, pixel filter, transparent background). An explicit transparency setting now overrides the automatic one. | `ff9948176` |
| Changing the device or thread count restarts the delegate. | `16f63cd20` |
| `shader_defaults.json` in the resource directory overrides Cycles socket defaults per node type. Houdini only writes parameters that differ from the UI default, so the delegate must use the same defaults as the UI. | `2595a4dd6` |

### Houdini user interface

| Change | Commit |
|---|---|
| **96 Cycles shader nodes** as VOP HDAs, plus a **USD Cycles Material Builder** shelf tool and icon (ported from the `shaders` branch, made to work on Windows) | `3653a9084` |
| **Shader nodes laid out like Blender 5.2:** Blender's order, labels, menus, collapsible panels, hide rules and default values. `blender_node_layouts.py` runs inside Blender and dumps how each node is drawn; `blender_layout.py` maps that onto the Cycles nodes. Parameter names are unchanged, so existing scenes still load. | `c9c2640e7` |
| **Slider ranges from Blender's node declarations,** read from the Blender source because the Python API only gives generic ranges. Fixes sliders from −10000 to 10000, and IOR inputs that were clamped below their defaults. | `702fd2b44` |
| **Cycles tab on the Render Settings LOP and in the viewport display options,** laid out like Blender's render properties: Sampling, Denoise, Path Guiding, Light Paths, Clamping, Caustics, Fast GI, Volumes, Film, Performance. Generated from the delegate's own settings, so defaults and menus always match. | `16f63cd20`, `de6ced211` |
| **Cycles Render Settings LOP:** one node that creates the render settings, product and var prims with every Cycles setting authored, like Karma's. | `8abcbdb5f` |

### Documentation

`BUILDING.md` gained a Houdini section: CUDA/OptiX options, static Embree, the Houdini license needed for install, and
where everything is in Houdini (`1ed26fc06`, `d8c47dbe5`, `be4a8103b`, `f6d08340b`). This README replaces the upstream
one (`38f5f148b`).

### New files

```
src/hydra/houdini/
  CMakeLists.txt                    install step: HDAs, render settings UI, LOP, package, shelf
  UsdRenderers.json                 registers "Cycles" with Houdini
  blender_node_layouts.json         Blender 5.2 shader node layouts (generated in Blender)
  packages/cycles.json.in           Houdini package file
  toolbar/cycles.shelf, icons/      USD Cycles Material Builder tool
  src/shaders.cpp                   dumps the Cycles shader node registry and render settings to JSON
  src/shader2hda.py                 builds the shader node HDAs
  src/blender_node_layouts.py       runs in Blender, dumps node layouts (+ ranges from Blender source)
  src/blender_layout.py             maps Blender layouts onto Cycles nodes
  src/rendersettings2ds.py          Cycles tab + viewport options (.ds)
  src/cyclesrendersettings2hda.py   Cycles Render Settings LOP
```

New Houdini-only source: `src/hydra/houdini_volume.{h,cpp}` (volumes from SOPs).

Modified upstream files: `src/hydra/{camera,display_driver,field,geometry.inl,light,material,render_delegate,render_pass,session}`,
`src/hydra/CMakeLists.txt`, `src/cmake/external_libs.cmake`, `src/cmake/Modules/FindUSDHoudini.cmake`,
`src/scene/shader_nodes.cpp`, `BUILDING.md`, `README.md`.

Full history: `git log main..houdini-22`.

## License and credits

Cycles is licensed under the Apache License 2.0, see [LICENSE](LICENSE). Cycles is developed by the Blender
Foundation and contributors ([AUTHORS](AUTHORS)); the Houdini Hydra integration builds on the work of
[cjhosken/hdCycles](https://projects.blender.org/cjhosken/hdCycles). Houdini is a product of SideFX; this project
is not affiliated with SideFX or the Blender Foundation.
