# Cycles for Nuke

`CyclesRender` renders a scene of Nuke's USD-based 3D system with Cycles, on NVIDIA GPUs
(OptiX, CUDA) or the CPU, with denoising. Connect a `GeoScene` (or any 3D node) to its input;
the camera is the first camera in the scene, or the prim path in the *camera* knob.

Supported: Nuke 15.0 (USD 23.05), Windows. Each Nuke release ships its own USD, so the plugin is
built once per Nuke version.

## Install

Add the build's install folder to `NUKE_PATH`, or to `~/.nuke/init.py`:

```python
nuke.pluginAddPath("C:/path/to/install-nuke15.0v1")
```

The node is in *3D > CyclesRender*.

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
`lib/legacy/windows_x64` (`git submodule update --init lib/legacy/windows_x64`).

Configure with:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DNUKE_ROOT="C:/Program Files/Nuke15.0v1" ^
  -DNUKE_USD_INCLUDE_DIR=usd-23.05 ^
  -DNUKE_BOOST_INCLUDE_DIR=boost_1_80_0 ^
  -DNUKE_IMPLIB_DIR=implibs-nuke15 ^
  -DWITH_CYCLES_HYDRA_RENDER_DELEGATE=ON ^
  -DWITH_CYCLES_DEVICE_OPTIX=ON -DOPTIX_ROOT_DIR=... ^
  -DCMAKE_INSTALL_PREFIX=install-nuke15.0v1
cmake --build build --target install
```

The install step renames bundled libraries that Nuke also loads in another version
(OpenEXR, Imath) with `rename_dlls.py`, so both can be loaded.

## Debugging

Set `NKCYCLES_DUMP_USD=<file>` to write the scene the node receives from Nuke as USD text.
