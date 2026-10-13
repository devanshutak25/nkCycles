/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include "util/unique_ptr.h"

#include "hydra/usd_compat.h"
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesMaterial final : public PXR_NS::HdMaterial {
 public:
  HdCyclesMaterial(const PXR_NS::SdfPath &sprimId);
  ~HdCyclesMaterial() override;

  PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const override;

  void Sync(PXR_NS::HdSceneDelegate *sceneDelegate,
            PXR_NS::HdRenderParam *renderParam,
            PXR_NS::HdDirtyBits *dirtyBits) override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

  CCL_NS::Shader *GetCyclesShader() const
  {
    return _shader;
  }

 private:
  struct NodeDesc {
    CCL_NS::ShaderNode *node;
    const class UsdToCyclesMapping *mapping;
  };

  void UpdateParameters(NodeDesc &nodeDesc,
                        PXR_NS::HdMaterialNodeParameterContainerSchema params,
                        const PXR_NS::SdfPath &nodePath);

  void UpdateConnections(NodeDesc &nodeDesc,
                         PXR_NS::HdMaterialNodeSchema nodeSchema,
                         const PXR_NS::SdfPath &nodePath,
                         CCL_NS::ShaderGraph *shaderGraph);

  unique_ptr<CCL_NS::ShaderGraph> BuildShaderGraph(PXR_NS::HdMaterialNetworkSchema network);

  CCL_NS::Shader *_shader = nullptr;
  std::unordered_map<PXR_NS::SdfPath, NodeDesc, PXR_NS::SdfPath::Hash> _nodes;
  /* Nodes left out of the graph that pass their input through, with the source of that input. */
  std::unordered_map<PXR_NS::SdfPath,
                     std::pair<PXR_NS::SdfPath, PXR_NS::TfToken>,
                     PXR_NS::SdfPath::Hash>
      _passThrough;
  /* Scale and bias of UsdUVTexture nodes, applied by multiply-add nodes after the outputs. */
  struct TextureScaleBias {
    PXR_NS::GfVec4f scale = PXR_NS::GfVec4f(1.0f);
    PXR_NS::GfVec4f bias = PXR_NS::GfVec4f(0.0f);
  };
  std::unordered_map<PXR_NS::SdfPath, TextureScaleBias, PXR_NS::SdfPath::Hash> _textureScaleBias;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
