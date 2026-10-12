/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "util/thread.h"

#include <pxr/imaging/hd/renderDelegate.h>

#include <optional>

HDCYCLES_NAMESPACE_OPEN_SCOPE

struct SceneLock {
  SceneLock(const PXR_NS::HdRenderParam *renderParam);
  ~SceneLock();

  CCL_NS::Scene *scene;

 private:
  CCL_NS::thread_scoped_lock sceneLock;
};

class HdCyclesSession final : public PXR_NS::HdRenderParam {
 public:
  HdCyclesSession(CCL_NS::Session *session_, const bool keep_nodes);
  HdCyclesSession(const CCL_NS::SessionParams &params);
  ~HdCyclesSession() override;

  void UpdateScene();

  // Override the automatic background transparency, which depends on presence of a dome light
  void SetTransparentBackground(const std::optional<bool> transparent);

  double GetStageMetersPerUnit() const
  {
    return _stageMetersPerUnit;
  }

  void SetStageMetersPerUnit(const double stageMetersPerUnit)
  {
    _stageMetersPerUnit = stageMetersPerUnit;
  }

  // Projection of the camera being rendered, to convert the depth pass to Hydra's depth AOV
  PXR_NS::GfMatrix4d GetProjectionMatrix() const
  {
    const CCL_NS::thread_scoped_lock lock(_projectionMutex);
    return _projectionMatrix;
  }

  void SetProjectionMatrix(const PXR_NS::GfMatrix4d &projectionMatrix)
  {
    const CCL_NS::thread_scoped_lock lock(_projectionMutex);
    _projectionMatrix = projectionMatrix;
  }

  // The Houdini viewport reads the depth AOV as window depth (0..1), final renders keep distance
  bool GetDepthToWindow() const
  {
    return _depthToWindow;
  }

  void SetDepthToWindow(const bool depthToWindow)
  {
    _depthToWindow = depthToWindow;
  }

  PXR_NS::HdRenderPassAovBinding GetDisplayAovBinding() const
  {
    return _displayAovBinding;
  }

  void SetDisplayAovBinding(const PXR_NS::HdRenderPassAovBinding &aovBinding)
  {
    _displayAovBinding = aovBinding;
  }

  const PXR_NS::HdRenderPassAovBindingVector &GetAovBindings() const
  {
    return _aovBindings;
  }

  void SyncAovBindings(const PXR_NS::HdRenderPassAovBindingVector &aovBindings);

  void RemoveAovBinding(PXR_NS::HdRenderBuffer *renderBuffer);

  CCL_NS::Session *session;
  bool keep_nodes;

 private:
  const bool _ownCyclesSession;
  double _stageMetersPerUnit = 0.01;
  PXR_NS::GfMatrix4d _projectionMatrix = PXR_NS::GfMatrix4d(1.0);
  mutable CCL_NS::thread_mutex _projectionMutex;
  bool _depthToWindow = false;
  std::optional<bool> _transparentBackground;
  PXR_NS::HdRenderPassAovBindingVector _aovBindings;
  PXR_NS::HdRenderPassAovBinding _displayAovBinding;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
