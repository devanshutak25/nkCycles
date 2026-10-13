/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "scene/attribute.h"

#include <algorithm>

#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/enums.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/timeSampleArray.h>
#include <pxr/imaging/hd/types.h>

PXR_NAMESPACE_OPEN_SCOPE
class HdSceneDelegate;
class SdfPath;
class TfToken;
PXR_NAMESPACE_CLOSE_SCOPE

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Prim data source from the scene index, or empty if none is reachable. */
PXR_NS::HdSceneIndexPrim GetPrim(PXR_NS::HdSceneDelegate *delegate, const PXR_NS::SdfPath &id);

/* Scene paths of the instances of a prim, like Hydra's picking gives them: the prim origins of the
 * native instances and of the prim, e.g. "/World/A/Box" and "/World/B/Box" for a prim of a native
 * instance prototype. Empty when the scene has no prim origins (USD imaging delegate). */
PXR_NS::SdfPathVector GetInstanceScenePaths(PXR_NS::HdSceneDelegate *delegate,
                                            const PXR_NS::SdfPath &id,
                                            size_t numInstances);

/* Typed child data source, or null if missing or of a different type. */
template<typename T>
typename PXR_NS::HdTypedSampledDataSource<T>::Handle GetTypedDataSource(
    const PXR_NS::HdContainerDataSourceHandle &container, const PXR_NS::TfToken &name)
{
  if (!container) {
    return nullptr;
  }
  return PXR_NS::HdTypedSampledDataSource<T>::Cast(container->Get(name));
}

/* Typed value at shutter offset 0, or `fallback` if missing or of a different type. */
template<typename T>
T GetTypedValue(const PXR_NS::HdContainerDataSourceHandle &container,
                const PXR_NS::TfToken &name,
                const T &fallback = T())
{
  if (!container) {
    return fallback;
  }
  const auto base = container->Get(name);
  if (const auto typed = PXR_NS::HdTypedSampledDataSource<T>::Cast(base)) {
    return typed->GetTypedValue(0.0f);
  }
  if (const auto sampled = PXR_NS::HdSampledDataSource::Cast(base)) {
    PXR_NS::VtValue v = sampled->GetValue(0.0f);
    if (v.IsHolding<T>()) {
      return v.UncheckedGet<T>();
    }
  }
  return fallback;
}

/* Values of a data source at the shutter offsets of the motion steps (HdCyclesSession::
 * motion_times), or only the value at offset 0 without motion blur or when it does not change
 * over the shutter. get(time) reads the value. */
template<typename T, typename Get>
std::vector<T> SampleMotion(const PXR_NS::HdSampledDataSourceHandle &ds,
                            const std::vector<float> &times,
                            const Get &get)
{
  std::vector<T> values;
  std::vector<float> sampleTimes;
  if (times.size() > 1 &&
      ds->GetContributingSampleTimesForInterval(times.front(), times.back(), &sampleTimes))
  {
    for (const float time : times) {
      values.push_back(get(time));
    }
    if (std::all_of(
            values.begin(), values.end(), [&](const T &value) { return value == values.front(); }))
    {
      values.clear();
    }
  }
  if (values.empty()) {
    values.push_back(get(0.0f));
  }
  return values;
}

/* Read a primvar's value at shutter offset 0. */
PXR_NS::VtValue ReadPrimvar(PXR_NS::HdPrimvarsSchema primvars, const PXR_NS::TfToken &name);

/* Read a primvar's interpolation. */
PXR_NS::HdInterpolation ReadPrimvarInterpolation(PXR_NS::HdPrimvarsSchema primvars,
                                                 const PXR_NS::TfToken &name);

/* Read a primvar's role token. */
PXR_NS::TfToken ReadPrimvarRole(PXR_NS::HdPrimvarsSchema primvars, const PXR_NS::TfToken &name);

/* Enumerate primvar names whose interpolation matches. */
PXR_NS::TfTokenVector PrimvarNamesAtInterpolation(PXR_NS::HdPrimvarsSchema primvars,
                                                  PXR_NS::HdInterpolation interpolation);

/* Convert a Hydra primvar value to a Cycles attribute. */
void ApplyPrimvars(CCL_NS::AttributeSet &attributes,
                   const CCL_NS::ustring &name,
                   PXR_NS::VtValue value,
                   CCL_NS::AttributeElement elem,
                   CCL_NS::AttributeStandard std);

HDCYCLES_NAMESPACE_CLOSE_SCOPE
