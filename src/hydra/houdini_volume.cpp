/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Built with the Houdini Development Kit definitions, and separate from the other sources to keep
 * Houdini headers out of them. */

#include "hydra/houdini_volume.h"

#include <GEO/GEO_PrimVDB.h>
#include <GT/GT_PrimVDB.h>

#include <cstdlib>
#include <mutex>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace HdCycles {

namespace {

/* Returns a GT_PrimVDB owned by Houdini. */
using GetVdbVolumePrimitiveFn = void *(*)(const char *filepath, const char *name);

GetVdbVolumePrimitiveFn get_vdb_volume_primitive_fn()
{
  static GetVdbVolumePrimitiveFn fn = nullptr;
  static std::once_flag once;
  std::call_once(once, [] {
    const char *hfs = std::getenv("HFS");
    if (!hfs) {
      return;
    }
#ifdef _WIN32
    const std::string library = std::string(hfs) + "/houdini/dso/USD_SopVol.dll";
    HMODULE module = LoadLibraryA(library.c_str());
    if (module) {
      fn = reinterpret_cast<GetVdbVolumePrimitiveFn>(
          GetProcAddress(module, "SOPgetVDBVolumePrimitive"));
    }
#else
#  ifdef __APPLE__
    const std::string library = std::string(hfs) + "/houdini/dso/USD_SopVol.dylib";
#  else
    const std::string library = std::string(hfs) + "/houdini/dso/USD_SopVol.so";
#  endif
    void *handle = dlopen(library.c_str(), RTLD_NOW);
    if (handle) {
      fn = reinterpret_cast<GetVdbVolumePrimitiveFn>(dlsym(handle, "SOPgetVDBVolumePrimitive"));
    }
#endif
  });
  return fn;
}

}  // namespace

bool IsHoudiniVolumePath(const std::string &path)
{
  return path.rfind("op:", 0) == 0;
}

openvdb::GridBase::ConstPtr LoadHoudiniVdbGrid(const std::string &path, const std::string &name)
{
  const GetVdbVolumePrimitiveFn fn = get_vdb_volume_primitive_fn();
  if (!fn) {
    return nullptr;
  }
  const GT_PrimVDB *prim = static_cast<const GT_PrimVDB *>(fn(path.c_str(), name.c_str()));
  if (!prim || !prim->getGeoPrimitive()) {
    return nullptr;
  }
  return prim->getGeoPrimitive()->getConstGridPtr();
}

}  // namespace HdCycles
