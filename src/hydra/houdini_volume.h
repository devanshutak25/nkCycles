/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <openvdb/openvdb.h>

#include <string>

namespace HdCycles {

/* Houdini references volumes from SOPs in a LOP stage with "op:" paths instead of files, and
 * provides them to render delegates through the USD_SopVol library. */
bool IsHoudiniVolumePath(const std::string &path);

/* Grid of a VDB volume primitive in a Houdini SOP network, or null when it is not found or not
 * running inside Houdini. The grid is shared with Houdini, which copies it before changing it. */
openvdb::GridBase::ConstPtr LoadHoudiniVdbGrid(const std::string &path, const std::string &name);

/* Native Houdini volume primitive (houdiniFieldAsset, from a SOP network or a .bgeo file),
 * converted to an OpenVDB grid. Null when it is not found or not running inside Houdini. */
openvdb::GridBase::ConstPtr LoadHoudiniVolumeGrid(const std::string &path,
                                                  const std::string &name,
                                                  int index);

}  // namespace HdCycles
