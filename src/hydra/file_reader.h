/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include "session/session.h"

#include "util/unique_ptr.h"

#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/timeCode.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesFileReader {
 public:
  static void read(Session *session, const char *filepath, const bool use_camera = true);
  /* Read a stage at the given time. Uses the camera at camera_path, or the first camera in
   * the stage when empty. */
  static void read(Session *session,
                   const PXR_NS::UsdStageRefPtr &stage,
                   PXR_NS::UsdTimeCode time,
                   const PXR_NS::SdfPath &camera_path,
                   const bool use_camera = true,
                   PXR_NS::CameraUtilConformWindowPolicy window_policy = PXR_NS::CameraUtilFit);

  /* Keep the Hydra scene of a stage, to sync only the changes of the stage the next times.
   * Without keep_nodes, the session nodes of the stage are removed with the reader. */
  HdCyclesFileReader(Session *session,
                     const PXR_NS::UsdStageRefPtr &stage,
                     double meters_per_unit,
                     bool keep_nodes = false);
  ~HdCyclesFileReader();

  /* Sync the stage changes since the last sync, at the given time. Returns false when
   * use_camera is set and there is no matching camera. */
  bool sync(PXR_NS::UsdTimeCode time,
            const PXR_NS::SdfPath &camera_path,
            const bool use_camera = true,
            PXR_NS::CameraUtilConformWindowPolicy window_policy = PXR_NS::CameraUtilFit);

 private:
  struct State;
  unique_ptr<State> state_;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
