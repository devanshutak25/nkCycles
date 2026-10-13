nkCycles: Cycles for Nuke
=========================

Blender's [Cycles](https://www.cycles-renderer.org) path tracer as a render node for **Nuke's USD-based 3D system**:
`CyclesRender` renders the scene of a `GeoScene` (or any 3D node) on NVIDIA GPUs (OptiX, CUDA) or the CPU, with
denoising, render passes and Cryptomatte.

> **Status: work in progress.** Tested with Nuke 15.0v1 on Windows 11 with an RTX 4090, in Nuke's terminal mode
> (`Nuke -t`). Not yet tried in the Nuke GUI. Other Nuke versions are not supported yet: each Nuke release ships its
> own USD, so the plugin is built once per Nuke version.

## Features

| Area | State |
|---|---|
| Devices | OptiX and CUDA (chosen automatically, OptiX > CUDA > CPU) and CPU, with denoising |
| Scenes | Meshes, cameras, `DirectLight1`, `PointLight`, `SpotLight1`, `EnvironmentLight` (with its image) and imported USD files |
| Materials | `PreviewSurface`, images bound directly as materials, Nuke images as textures |
| Passes | Depth (1/z, like ScanlineRender), normal, position, albedo, Blender-style light passes, Cryptomatte for Nuke's Cryptomatte node |
| Interaction | The Cycles session is kept between renders: changing a knob only updates what changed |
| Images | Any size (large images are rendered in tiles) |

Known limits:
- No motion blur yet, and no camera window (`win_translate`, `win_scale`) knobs.
- `BasicSurface` and `Project3DShader` materials are not translated yet.
- Cryptomatte names of instanced prims appear once the scene has been rendered (their IDs always work).
- Windows only for now.

## Install and use

There are no releases yet: build the plugin from source as described in [src/nuke/README.md](src/nuke/README.md),
then add the install folder to `NUKE_PATH`. The node is in *3D > CyclesRender*. Its knobs, the material and light
support and the passes are described in [src/nuke/README.md](src/nuke/README.md).

## Repository

- `src/nuke`: the render node, its build files and documentation.
- `src/hydra`: the Hydra render delegate code the node uses to read USD scenes. It is shared with hdCycles, Cycles
  for Houdini, whose documentation is in [src/hydra/houdini/README.md](src/hydra/houdini/README.md).
- Everything else is Cycles, from Blender's repository.

This is based on [Blender's Cycles repository](https://projects.blender.org/blender/cycles) and
[cjhosken/hdCycles](https://projects.blender.org/cjhosken/hdCycles), through
[hdCycles for Houdini 22](https://projects.blender.org/devanshutak25/hdCycles-h22).

## License and notices

Cycles is licensed under the Apache License 2.0, see [LICENSE](LICENSE), with third-party licenses in
[src/doc/license](src/doc/license).

Nuke is a trademark of The Foundry Visionmongers Ltd. This project is not affiliated with or endorsed by Foundry or
the Blender Foundation. Nuke's NDK headers and libraries are not included: building needs a Nuke installation. The
NVIDIA OptiX SDK and the CUDA toolkit are obtained separately.
