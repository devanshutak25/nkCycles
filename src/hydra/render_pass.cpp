/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/render_pass.h"
#include "hydra/camera.h"
#include "hydra/output_driver.h"
#include "hydra/render_buffer.h"
#include "hydra/render_delegate.h"
#include "hydra/session.h"

#ifdef WITH_HYDRA_DISPLAY_DRIVER
#  include "hydra/display_driver.h"
#endif

#include "scene/camera.h"
#include "scene/integrator.h"
#include "scene/scene.h"

#include "session/session.h"

#include <pxr/imaging/hd/renderPassState.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

HdCyclesRenderPass::HdCyclesRenderPass(HdRenderIndex *index,
                                       const HdRprimCollection &collection,
                                       HdCyclesSession *renderParam)
    : HdRenderPass(index, collection), _renderParam(renderParam)
{
  Session *const session = _renderParam->session;
  // Reset cancel state so session thread can continue rendering
  session->progress.reset();

  session->set_output_driver(make_unique<HdCyclesOutputDriver>(renderParam));

  const auto *const renderDelegate = static_cast<const HdCyclesDelegate *>(
      GetRenderIndex()->GetRenderDelegate());
  if (renderDelegate->IsDisplaySupported()) {
#ifdef WITH_HYDRA_DISPLAY_DRIVER
    session->set_display_driver(
        make_unique<HdCyclesDisplayDriver>(renderParam, renderDelegate->GetHgi()));
#endif
  }
}

HdCyclesRenderPass::~HdCyclesRenderPass()
{
  Session *const session = _renderParam->session;
  session->cancel(true);
}

bool HdCyclesRenderPass::IsConverged() const
{
  for (const HdRenderPassAovBinding &aovBinding : _renderParam->GetAovBindings()) {
    if (aovBinding.renderBuffer && !aovBinding.renderBuffer->IsConverged()) {
      return false;
    }
  }

  return true;
}

void HdCyclesRenderPass::ResetConverged()
{
  for (const HdRenderPassAovBinding &aovBinding : _renderParam->GetAovBindings()) {
    if (auto *const renderBuffer = static_cast<HdCyclesRenderBuffer *>(aovBinding.renderBuffer)) {
      renderBuffer->SetConverged(false);
    }
  }
}

void HdCyclesRenderPass::_Execute(const HdRenderPassStateSharedPtr &renderPassState,
                                  const TfTokenVector & /*renderTags*/)
{
  Scene *const scene = _renderParam->session->scene.get();
  Session *const session = _renderParam->session;

  if (session->progress.get_cancel()) {
    return;  // Something went wrong and cannot continue without recreating the session
  }

  if (scene->mutex.try_lock()) {
    auto *const renderDelegate = static_cast<HdCyclesDelegate *>(
        GetRenderIndex()->GetRenderDelegate());

    const unsigned int settingsVersion = renderDelegate->GetRenderSettingsVersion();

    // Update requested AOV bindings
    const HdRenderPassAovBindingVector &aovBindings = renderPassState->GetAovBindings();
    if (_renderParam->GetAovBindings() != aovBindings ||
        // Need to resync passes when denoising is enabled or disabled to update the pass mode
        (settingsVersion != _lastSettingsVersion && scene->integrator->use_denoise_is_modified()))
    {
      _renderParam->SyncAovBindings(aovBindings);

      if (renderDelegate->IsDisplaySupported()) {
        // Update display pass to the first requested color AOV
        const HdRenderPassAovBinding displayAovBinding = !aovBindings.empty() ?
                                                             aovBindings.front() :
                                                             HdRenderPassAovBinding();
        if (displayAovBinding.aovName == HdAovTokens->color && displayAovBinding.renderBuffer) {
          _renderParam->SetDisplayAovBinding(displayAovBinding);
        }
        else {
          _renderParam->SetDisplayAovBinding(HdRenderPassAovBinding());
        }
      }
    }

    // Update camera dimensions to the viewport size
    CameraUtilFraming framing = renderPassState->GetFraming();
    float aspectRatio = 0.0f;
    if (!framing.IsValid()) {
      const GfVec4f vp = renderPassState->GetViewport();
      GfVec2i size(static_cast<int>(vp[2]), static_cast<int>(vp[3]));

      // The render buffers can have a different size than the viewport. Houdini binds buffers
      // the size of the whole viewer while looking through a camera, sets the viewport to the
      // size of the camera gate, and stretches the buffers over the gate. Render at the buffer
      // size, since the display only draws into a buffer of the same size as the render, with
      // the camera window conformed to the aspect ratio of the viewport.
      for (const HdRenderPassAovBinding &aovBinding : aovBindings) {
        if (aovBinding.renderBuffer && aovBinding.renderBuffer->GetWidth() != 0 &&
            aovBinding.renderBuffer->GetHeight() != 0)
        {
          const GfVec2i bufferSize(int(aovBinding.renderBuffer->GetWidth()),
                                   int(aovBinding.renderBuffer->GetHeight()));
          if (bufferSize != size && vp[2] > 0.0f && vp[3] > 0.0f) {
            aspectRatio = vp[2] / vp[3];
            size = bufferSize;
          }
          break;
        }
      }

      framing = CameraUtilFraming(GfRect2i(GfVec2i(0), size[0], size[1]));
    }

    scene->camera->set_full_width(framing.dataWindow.GetWidth());
    scene->camera->set_full_height(framing.dataWindow.GetHeight());

    if (const auto *const camera = static_cast<const HdCyclesCamera *>(
            renderPassState->GetCamera()))
    {
      camera->ApplyCameraSettings(_renderParam, scene->camera, aspectRatio);
    }
    else {
      HdCyclesCamera::ApplyCameraSettings(_renderParam,
                                          renderPassState->GetWorldToViewMatrix(),
                                          renderPassState->GetProjectionMatrix(),
                                          renderPassState->GetClipPlanes(),
                                          scene->camera,
                                          aspectRatio);
    }

    // Reset session if the session, scene, camera or AOV bindings changed
    if (scene->need_reset() || settingsVersion != _lastSettingsVersion) {
      _lastSettingsVersion = settingsVersion;

      // Reset convergence state of all render buffers
      ResetConverged();

      BufferParams buffer_params;
      buffer_params.full_x = static_cast<int>(framing.displayWindow.GetMin()[0]);
      buffer_params.full_y = static_cast<int>(framing.displayWindow.GetMin()[1]);
      buffer_params.full_width = static_cast<int>(framing.displayWindow.GetSize()[0]);
      buffer_params.full_height = static_cast<int>(framing.displayWindow.GetSize()[1]);

      buffer_params.window_x = framing.dataWindow.GetMinX() - buffer_params.full_x;
      buffer_params.window_y = framing.dataWindow.GetMinY() - buffer_params.full_y;
      buffer_params.window_width = framing.dataWindow.GetWidth();
      buffer_params.window_height = framing.dataWindow.GetHeight();

      buffer_params.width = buffer_params.window_width;
      buffer_params.height = buffer_params.window_height;

      session->reset(session->params, buffer_params);
    }

    scene->mutex.unlock();

    // Start Cycles render thread if not already running
    session->start();
  }

  session->draw();
}

void HdCyclesRenderPass::_MarkCollectionDirty() {}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
