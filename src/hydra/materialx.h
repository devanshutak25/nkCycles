/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include "util/unique_ptr.h"

#include <pxr/imaging/hd/materialNetworkSchema.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Render contexts of MaterialX material networks, in order of preference: plain MaterialX
 * (USD "mtlx:surface" outputs) and Karma (Houdini's Karma Material Builder, "kma:surface"). */
const PXR_NS::TfTokenVector &MaterialXRenderContexts();

/* Whether the terminals of a network connect to MaterialX nodes ("ND_..." node ids). */
bool IsMaterialXNetwork(const PXR_NS::HdMaterialNetworkSchema &network);

/* Translate a MaterialX material network (shader nodes with MaterialX node definition ids, like
 * "ND_standard_surface_surfaceshader") to a Cycles shader graph. Surface shaders are translated
 * to the Principled BSDF, other nodes to their Cycles equivalents, and nodes that are defined as
 * node graphs in the MaterialX libraries are expanded. Returns nothing if no terminal could be
 * translated. */
CCL_NS::unique_ptr<CCL_NS::ShaderGraph> BuildMaterialXShaderGraph(
    const PXR_NS::SdfPath &materialId,
    const PXR_NS::HdMaterialNetworkSchema &network,
    const PXR_NS::HdContainerDataSourceHandle &primContainer);

HDCYCLES_NAMESPACE_CLOSE_SCOPE
