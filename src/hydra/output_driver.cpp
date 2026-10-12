/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/output_driver.h"
#include "hydra/render_buffer.h"
#include "hydra/session.h"

#include <algorithm>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

// Cycles writes the distance along the camera axis in meters (1e10 for background), Hydra expects
// window depth in 0..1 like an OpenGL depth buffer. Houdini uses it for the tumble pivot.
void convert_depth(float *depth,
                   const size_t num_pixels,
                   const GfMatrix4d &projection,
                   const double metersPerUnit)
{
  for (size_t i = 0; i < num_pixels; ++i) {
    if (depth[i] >= 1e9f) {
      depth[i] = 1.0f;
      continue;
    }
    // Point on the view axis at that distance (camera looks down -Z), in stage units
    const double z = -depth[i] / metersPerUnit;
    const double clip_z = z * projection[2][2] + projection[3][2];
    const double clip_w = z * projection[2][3] + projection[3][3];
    const double ndc_z = clip_w != 0.0 ? clip_z / clip_w : 1.0;
    depth[i] = float(std::clamp(ndc_z * 0.5 + 0.5, 0.0, 1.0));
  }
}

}  // namespace

HdCyclesOutputDriver::HdCyclesOutputDriver(HdCyclesSession *renderParam)
    : _renderParam(renderParam)
{
}

void HdCyclesOutputDriver::write_render_tile(const Tile &tile)
{
  update_render_tile(tile);

  // Update convergence state of all render buffers
  for (const HdRenderPassAovBinding &aovBinding : _renderParam->GetAovBindings()) {
    if (auto *const renderBuffer = static_cast<HdCyclesRenderBuffer *>(aovBinding.renderBuffer)) {
      renderBuffer->SetConverged(true);
    }
  }
}

bool HdCyclesOutputDriver::update_render_tile(const Tile &tile)
{
  std::vector<float> pixels;

  for (const HdRenderPassAovBinding &aovBinding : _renderParam->GetAovBindings()) {
    if (auto *const renderBuffer = static_cast<HdCyclesRenderBuffer *>(aovBinding.renderBuffer)) {
      if (aovBinding == _renderParam->GetDisplayAovBinding() && renderBuffer->IsResourceUsed()) {
        continue;  // Display AOV binding is already updated by Cycles display driver
      }

      const HdFormat format = renderBuffer->GetFormat();
      if (format == HdFormatInvalid) {
        continue;  // Skip invalid AOV bindings
      }

      const size_t channels = HdGetComponentCount(format);
      // Avoid extra copy by mapping render buffer directly when dimensions/format match the tile
      if (tile.offset.x == 0 && tile.offset.y == 0 && tile.size.x == renderBuffer->GetWidth() &&
          tile.size.y == renderBuffer->GetHeight() &&
          (format >= HdFormatFloat32 && format <= HdFormatFloat32Vec4))
      {
        float *const data = static_cast<float *>(renderBuffer->Map());
        TF_VERIFY(
            tile.get_pass_pixels(_renderParam->GetPassName(aovBinding.aovName), channels, data));
        if (aovBinding.aovName == HdAovTokens->depth && channels == 1 &&
            _renderParam->GetDepthToWindow()) {
          convert_depth(data,
                        tile.size.x * tile.size.y,
                        _renderParam->GetProjectionMatrix(),
                        _renderParam->GetStageMetersPerUnit());
        }
        renderBuffer->Unmap();
      }
      else {
        pixels.resize(channels * tile.size.x * tile.size.y);
        if (tile.get_pass_pixels(
                _renderParam->GetPassName(aovBinding.aovName), channels, pixels.data()))
        {
          if (aovBinding.aovName == HdAovTokens->depth && channels == 1 &&
            _renderParam->GetDepthToWindow()) {
            convert_depth(pixels.data(),
                          tile.size.x * tile.size.y,
                          _renderParam->GetProjectionMatrix(),
                          _renderParam->GetStageMetersPerUnit());
          }
          const bool isId = aovBinding.aovName == HdAovTokens->primId ||
                            aovBinding.aovName == HdAovTokens->elementId ||
                            aovBinding.aovName == HdAovTokens->instanceId;
          renderBuffer->Map();
          renderBuffer->WritePixels(pixels.data(),
                                    GfVec2i(tile.offset.x, tile.offset.y),
                                    GfVec2i(tile.size.x, tile.size.y),
                                    channels,
                                    isId);
          renderBuffer->Unmap();
        }
        else {
          // Do not warn on missing elementId, which is a standard AOV but is not implememted
          if (aovBinding.aovName != HdAovTokens->elementId) {
            TF_RUNTIME_ERROR("Could not find pass for AOV '%s'", aovBinding.aovName.GetText());
          }
        }
      }
    }
  }

  return true;
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
