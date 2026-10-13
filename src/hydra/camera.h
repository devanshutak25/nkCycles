/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/base/gf/camera.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/timeSampleArray.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesCamera final : public PXR_NS::HdCamera {
 public:
  HdCyclesCamera(const PXR_NS::SdfPath &sprimId);
  ~HdCyclesCamera() override;

  /* The camera window is conformed to the aspect ratio of the image, or to the given aspect
   * ratio when it is not zero (for images displayed with non-square pixels). */
  void ApplyCameraSettings(PXR_NS::HdRenderParam *renderParam,
                           CCL_NS::Camera *cam,
                           float aspectRatio = 0.0f) const;

  static void ApplyCameraSettings(PXR_NS::HdRenderParam *renderParam,
                                  const PXR_NS::GfCamera &dataUnconformedWindow,
                                  PXR_NS::CameraUtilConformWindowPolicy windowPolicy,
                                  CCL_NS::Camera *cam,
                                  float aspectRatio = 0.0f);
  static void ApplyCameraSettings(PXR_NS::HdRenderParam *renderParam,
                                  const PXR_NS::GfMatrix4d &worldToViewMatrix,
                                  const PXR_NS::GfMatrix4d &projectionMatrix,
                                  const std::vector<PXR_NS::GfVec4d> &clipPlanes,
                                  CCL_NS::Camera *cam,
                                  float aspectRatio = 0.0f);

  /* Override how the camera window is conformed to the image aspect ratio. */
  void SetWindowPolicy(const PXR_NS::CameraUtilConformWindowPolicy windowPolicy)
  {
    _windowPolicy = windowPolicy;
  }

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits) override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

 private:
  PXR_NS::GfCamera _data;
  /* Transforms of the motion steps, or one without motion. */
  std::vector<PXR_NS::GfMatrix4d> _transforms;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
