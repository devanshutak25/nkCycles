/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Built with the Houdini Development Kit definitions, and separate from the other sources to keep
 * Houdini headers out of them. */

#include "hydra/houdini_volume.h"

#include <GEO/GEO_PrimVDB.h>
#include <GEO/GEO_PrimVolume.h>
#include <GT/GT_PrimVDB.h>
#include <GT/GT_PrimVolume.h>
#include <GU/GU_Detail.h>
#include <GU/GU_PrimVDB.h>

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
/* Returns a GT_PrimVolume owned by Houdini. */
using GetHoudiniVolumePrimitiveFn = void *(*)(const char *filepath, const char *name, int index);

/* Function exported by Houdini's USD_SopVol library. */
void *find_sop_volume_function(const char *function)
{
  static void *module = nullptr;
  static std::once_flag once;
  std::call_once(once, [] {
    const char *hfs = std::getenv("HFS");
    if (!hfs) {
      return;
    }
#ifdef _WIN32
    const std::string library = std::string(hfs) + "/houdini/dso/USD_SopVol.dll";
    module = LoadLibraryA(library.c_str());
#else
#  ifdef __APPLE__
    const std::string library = std::string(hfs) + "/houdini/dso/USD_SopVol.dylib";
#  else
    const std::string library = std::string(hfs) + "/houdini/dso/USD_SopVol.so";
#  endif
    module = dlopen(library.c_str(), RTLD_NOW);
#endif
  });
  if (!module) {
    return nullptr;
  }
#ifdef _WIN32
  return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(module), function));
#else
  return dlsym(module, function);
#endif
}

}  // namespace

bool IsHoudiniVolumePath(const std::string &path)
{
  return path.rfind("op:", 0) == 0;
}

openvdb::GridBase::ConstPtr LoadHoudiniVdbGrid(const std::string &path, const std::string &name)
{
  static const auto fn = reinterpret_cast<GetVdbVolumePrimitiveFn>(
      find_sop_volume_function("SOPgetVDBVolumePrimitive"));
  if (!fn) {
    return nullptr;
  }
  const GT_PrimVDB *prim = static_cast<const GT_PrimVDB *>(fn(path.c_str(), name.c_str()));
  if (!prim || !prim->getGeoPrimitive()) {
    return nullptr;
  }
  return prim->getGeoPrimitive()->getConstGridPtr();
}

openvdb::GridBase::ConstPtr LoadHoudiniVolumeGrid(const std::string &path,
                                                  const std::string &name,
                                                  const int index)
{
  static const auto fn = reinterpret_cast<GetHoudiniVolumePrimitiveFn>(
      find_sop_volume_function("SOPgetHoudiniVolumePrimitive"));
  if (!fn) {
    return nullptr;
  }
  const GT_PrimVolume *prim = static_cast<const GT_PrimVolume *>(
      fn(path.c_str(), name.c_str(), index));
  if (!prim) {
    return nullptr;
  }
  const GEO_PrimVolume *volume = dynamic_cast<const GEO_PrimVolume *>(prim->getGeoPrimitive());
  if (!volume) {
    return nullptr;
  }
  /* Cycles only renders OpenVDB grids. The grid keeps its data after the detail is gone. */
  GU_Detail detail;
  const GU_PrimVDB *vdb = GU_PrimVDB::buildFromPrimVolume(detail, *volume, name.c_str());
  if (!vdb) {
    return nullptr;
  }
  return vdb->getConstGridPtr();
}

}  // namespace HdCycles
