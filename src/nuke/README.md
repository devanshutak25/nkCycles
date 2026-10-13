# Cycles for Nuke

`CyclesRender` renders a scene of Nuke's USD-based 3D system with Cycles, on NVIDIA GPUs
(OptiX, CUDA) or the CPU, with denoising. Connect a `GeoScene` (or any 3D node) to its input;
the camera is the first camera in the scene, or the prim path in the *camera* knob.

Supported: Nuke 15.0 (USD 23.05) and Nuke 17.0 (USD 25.08), Windows. Each Nuke release ships
its own USD, so the plugin is built once per Nuke version. Nuke Indie and Nuke Non-commercial
cannot load C++ plugins: the node needs Nuke, NukeX or Nuke Studio.

## Install

Add the build's install folder to `NUKE_PATH`, or to `~/.nuke/init.py`:

```python
nuke.pluginAddPath("C:/path/to/install-nuke15.0v1")
```

The node is in *3D > CyclesRender*.

## Usage

- **Materials:** `PreviewSurface`, `BasicSurface`, `Project3DShader`, `MergeLayerShader`,
  `BlendSurface` (its opacity), `FillShader`, `ConstantShader`, and images bound directly with
  `GeoBindMaterial` (lit by the scene lights). Images connected to material inputs are used as
  textures, the color knobs multiply them. `Project3DShader` projects like Nuke (crop, front or
  back faces, clipping planes, the projector's window), without occlusion. `BasicSurface` maps
  its Phong specular to a softer one, without its shininess map and B input. Images without
  alpha are opaque. `WireframeShader` is not supported.
- **Lights:** `DirectLight1`, `PointLight`, `SpotLight1` and `EnvironmentLight` (with its image).
  Intensities follow USD lights: a `DirectLight1` gives the light of a sun disc of its angle, so
  turn *normalize* on to make the intensity the irradiance. Like ScanlineRender2, the background
  stays transparent: an `EnvironmentLight` lights the scene and shows in reflections only.
- **Holdout:** prim paths (separated by spaces) whose objects are cut out of the render, with
  alpha 0, while they still cast shadows and show in reflections.
- **Outputs:** like ScanlineRender, *depth* (1/z in scene units, 0 for the background,
  `depth.Z` by default), *normal* and *position* (world space) and *albedo* go to the channels
  picked in their knobs. The light pass toggles add layers named like Blender's, which add up
  to the (not denoised) beauty:
  `(DiffDir + DiffInd) * DiffCol + (GlossDir + GlossInd) * GlossCol +
  (TransDir + TransInd) * TransCol + VolumeDir + VolumeInd + Emit + Env`.
  The cryptomatte toggles add `CryptoObject00`, ... layers with their metadata, for Nuke's
  Cryptomatte node.
- **Depth of field** uses the camera's focal distance and f-stop when turned on, with the lens
  in millimeters and the scene in meters (Nuke 17: in the units of the camera's *world to
  meters*). Cameras of imported USD files use their own f-stop. When turned off there is none,
  even if the scene has an f-stop.

- **Cameras:** the window knobs (*win_translate*, *win_scale*, *winroll*) and orthographic
  cameras like the classic ScanlineRender. A roll with a different x and y window scale is a
  rotation here, a shear in Nuke.
- **Motion blur:** *motion blur steps* (0 off) and the *shutter* knobs like ScanlineRender2.
  Cameras, transforms and deforming meshes blur; instances move with their prototype only, and
  curves and points do not blur. A FrameHold before the node holds the motion too. In Nuke 15 the
  scene is read at the shutter open, center and close only.

The render is kept for all layers of a frame, and the Cycles session between renders, so
changing a light or a knob only updates what changed.

## Build

Nuke ships USD libraries but no USD headers. Prepare headers of the OpenUSD release Nuke was
built with:

```
git clone --depth 1 -b v23.05 https://github.com/PixarAnimationStudios/OpenUSD.git usd-23.05
python src/nuke/prepare_usd_headers.py 15 usd-23.05
```

Boost 1.80 headers are needed too (USD 23.05 headers use Boost; Nuke renames its namespace to
`foundryboost`, so make a `foundryboost` link to the `boost` folder). Nuke has no import
libraries for Python and Boost.Python, generate them with:

```
pwsh src/nuke/make_implibs.ps1 "C:/Program Files/Nuke15.0v1" implibs-nuke15
```

Nuke 15 uses TBB 2020, so the legacy precompiled libraries are needed in
`lib/legacy/windows_x64`: get them with `make.bat update_legacy`.

Configure with:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DNUKE_ROOT="C:/Program Files/Nuke15.0v1" ^
  -DNUKE_USD_INCLUDE_DIR=usd-23.05 ^
  -DNUKE_BOOST_INCLUDE_DIR=boost_1_80_0 ^
  -DNUKE_IMPLIB_DIR=implibs-nuke15 ^
  -DWITH_CYCLES_HYDRA_RENDER_DELEGATE=ON ^
  -DWITH_CYCLES_DEVICE_OPTIX=ON -DOPTIX_ROOT_DIR=... ^
  -DWITH_CYCLES_OSL=OFF ^
  -DCMAKE_INSTALL_PREFIX=install-nuke15.0v1
cmake --build build --target install
```

For Nuke 17, which ships its USD headers and libraries (`FnUSD`) and whose USD needs no Boost,
leave out `NUKE_USD_INCLUDE_DIR` and `NUKE_BOOST_INCLUDE_DIR`. Nuke 17 uses oneTBB 2021.13, like
the precompiled libraries of Blender 5.0: get them from
`https://projects.blender.org/blender/lib-windows_x64.git` (branch `blender-v5.0-release`) and
add `-DCYCLES_LIB_DIR=<that folder>`. The Python import library is still generated with
`make_implibs.ps1`.

OSL is not used by the node. A static Embree (`-DEMBREE_ROOT_DIR=...`) avoids installing the
precompiled shared one, which brings the SYCL runtime.

The install step copies the libraries the node uses, and renames bundled libraries that Nuke
also loads in another version (OpenEXR, Imath, OpenVDB) with `rename_dlls.py`, so both can be
loaded.
Install into an empty folder: files of earlier installs are not removed.

## Debugging

Set `NKCYCLES_DUMP_USD=<file.usda>` to write the scene that is rendered: the scene from Nuke,
with the light and camera knob values Nuke does not write into it.
