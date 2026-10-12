Building Cycles
===============

## Prerequisites

Ensure the following software is installed and available in the PATH:
- Git
- Git LFS
- Python 3
- CMake

### Standalone Build

Get the source code:

    git clone https://projects.blender.org/blender/cycles.git
    cd cycles

Download precompiled libraries and build:

    make update
    make

The resulting binary will be at:

    ./install/cycles

## Hydra Render Delegate with USD Repository

This will make the render delegate work with usdview and other applications built using the USD repository. USD version 25.11 or newer is required.


USD includes a script to build itself and all required dependencies and then install the result a specified directory.

    git clone https://github.com/PixarAnimationStudios/USD.git
    cd USD
    python3 build_scripts/build_usd.py "<path to USD install>"

Get the Cycles source code:

    git clone https://projects.blender.org/blender/cycles.git
    cd cycles

At the moment, when using USD from the USD repository, Cycles will build without OSL and NanoVDB support.
To build Cycles, download the Cycles dependency libraries and point CMake to the USD directory like this.

    make update
    cmake -B ./build -DPXR_ROOT="<path to USD install>"
    make

Test in usdview.

    PYTHONPATH=<path to USD install>/lib/python PXR_PLUGINPATH_NAME=<path to cycles>/install/hydra <path to USD install>/bin/usdview

## Hydra Render Delegate for Houdini

For use in Houdini, Cycles must be built using Houdini's USD libraries. Houdini version 21+ is required. 

Get the source code:

    git clone https://projects.blender.org/blender/cycles.git
    cd cycles

Download precompiled libraries and build.

Linux:
```
    make update
    cmake -B ./build -DHOUDINI_ROOT="<path to Houdini>"
    cmake --build build --config Release
    cmake --install build
```

Windows:
```
    .\make.bat update
    cmake -B .\build -DHOUDINI_ROOT="<path to Houdini>"
    cmake --build build --config Release
    cmake --install build
```

To build with NVIDIA GPU (CUDA and OptiX) rendering support, install the CUDA Toolkit and the OptiX SDK
headers (OptiX 8.0 or newer, for example from https://github.com/NVIDIA/optix-dev), and add these options to
the configure step:

    -DWITH_CYCLES_CUDA_BINARIES=ON
    -DOPTIX_ROOT_DIR="<path to OptiX SDK>"
    -DCYCLES_CUDA_BINARIES_ARCH="sm_75;sm_86;sm_89;sm_120;compute_75"

By default the best available GPU is used (OptiX, then CUDA), falling back to the CPU. The device can be chosen
with the `cycles:device` render setting (`AUTO`, `OPTIX`, `CUDA`, `CPU`) or the `CYCLES_DEVICE` environment
variable, for example `CYCLES_DEVICE=CPU husk -R Cycles scene.usd`.

On Windows, the precompiled Embree library depends on the SYCL runtime, which conflicts with other Houdini
plugins that ship their own SYCL runtime (for example Arnold), making the Cycles plugin fail to load. To avoid
this, build Embree as a static library without SYCL (`EMBREE_STATIC_LIB=ON`, `EMBREE_SYCL_SUPPORT=OFF`) and
point the configure step to it:

    -DEMBREE_ROOT_DIR="<path to Embree install>"

The install step runs Houdini's `hython` to generate the Cycles shader nodes and the render settings user
interface, so it requires a Houdini license.

The path to Houdini depends on the operating system, typically:
- Linux: `/opt/hfsX.Y`
- macOS: `/Applications/Houdini/HoudiniX.Y.ZZZ`
- Windows: `C:/Program Files/Side Effects Software/Houdini X.Y.ZZZ`

Test in Houdini using an environment variable.

    PXR_PLUGINPATH_NAME=<path to cycles>/install/houdini/dso/usd_plugins houdini

Or copy `install/houdini/packages/cycles.json` to the Houdini packages directory to make it always available.
The packages directory can be found (or needs to be created) under:
- Linux: `/home/[username]/houdiniX.Y`
- macOS: `/Users/[username]/Library/Preferences/houdini/X.Y`
- Windows: `C:/Users/[username]/Documents/houdiniX.Y`
**Note:** If you move the Cycles installation folder to another place you need to adjust the path in `cycles.json`.

In Houdini, select Cycles in the renderer menu of the Solaris viewport. Render settings are in the Cycles
Render Settings LOP, the Cycles tab of the Render Settings LOP and the viewport display options, laid out like
Blender's Cycles render properties. Cycles shader nodes are available in the Material Library LOP through the
USD Cycles Material Builder, laid out like Blender's shader nodes and with Blender's default values.

The shader node layouts come from `src/hydra/houdini/blender_node_layouts.json`. To update it for a new
Blender version, run `src/hydra/houdini/src/blender_node_layouts.py` in that Blender version (see the script).

## Build System

Cycles uses the CMake build system. As an alternative to the `make` wrapper, CMake can be manually configured.

See the CMake configuration to enable and disable various features.

The precompiled libraries are shared with Blender, and will be automatically downloaded from the Blender repository with `make update`. This will populate a submodule in the `lib/` folder, matching the platform.

## Dependencies

Core Cycles has the following required and optional library dependencies. These are all included in precompiled libraries.

Required:
- OpenImageIO
- TBB

Optional:
- Alembic
- Embree
- OpenColorIO
- OpenVDB / NanoVDB
- OpenShadingLanguage
- OpenImageDenoise
- USD

For GUI support, the following libraries are required. The SDL library must be manually provided, it's not part of the precompiled libraries.
- OpenGL
- GLEW
- SDL

For GPU rendering support on NVIDIA cards, these need to be downloaded and installed from the NVIDIA website.
- CUDA Toolkit 11 or newer
- OptiX 7.3 SDK or newer
