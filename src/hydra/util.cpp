/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/util.h"
#include "scene/attribute.h"

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/primOriginSchema.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

HdSceneIndexPrim GetPrim(HdSceneDelegate *delegate, const SdfPath &id)
{
  if (!delegate) {
    return {};
  }
  HdSceneIndexBaseRefPtr si = delegate->GetRenderIndex().GetTerminalSceneIndex();
  if (!si) {
    return {};
  }
  return si->GetPrim(id);
}

/* Append a prim origin scene path: absolute paths replace, relative ones are inside the path. */
static void AppendScenePath(const HdContainerDataSourceHandle &primDs, SdfPath &path)
{
  const SdfPath scenePath = HdPrimOriginSchema::GetFromParent(primDs).GetOriginPath(
      HdPrimOriginSchemaTokens->scenePath);
  if (!scenePath.IsEmpty()) {
    path = scenePath.IsAbsolutePath() ? scenePath : path.AppendPath(scenePath);
  }
}

SdfPathVector GetInstanceScenePaths(HdSceneDelegate *delegate,
                                    const SdfPath &id,
                                    const size_t numInstances)
{
  HdSceneIndexBaseRefPtr si = delegate ? delegate->GetRenderIndex().GetTerminalSceneIndex() :
                                         nullptr;
  if (!si) {
    return {};
  }
  const HdContainerDataSourceHandle primDs = si->GetPrim(id).dataSource;
  if (HdPrimOriginSchema::GetFromParent(primDs)
          .GetOriginPath(HdPrimOriginSchemaTokens->scenePath)
          .IsEmpty())
  {
    return {};
  }

  /* Instancers from the innermost: the instance indices of what they instance, and the paths of
   * their native instances. */
  struct Instancer {
    VtIntArray indices;
    VtArray<SdfPath> locations;
  };
  std::vector<Instancer> instancers;
  SdfPath path = id;
  HdContainerDataSourceHandle ds = primDs;
  while (true) {
    SdfPath instancerId;
    if (auto pathsDs = HdInstancedBySchema::GetFromParent(ds).GetPaths()) {
      const VtArray<SdfPath> paths = pathsDs->GetTypedValue(0.0f);
      if (!paths.empty()) {
        instancerId = paths.front();
      }
    }
    if (instancerId.IsEmpty()) {
      break;
    }
    ds = si->GetPrim(instancerId).dataSource;
    HdInstancerTopologySchema topology = HdInstancerTopologySchema::GetFromParent(ds);
    Instancer instancer;
    instancer.indices = topology ? topology.ComputeInstanceIndicesForProto(path) : VtIntArray();
    if (instancer.indices.empty()) {
      break;
    }
    if (auto locationsDs = topology.GetInstanceLocations()) {
      instancer.locations = locationsDs->GetTypedValue(0.0f);
    }
    instancers.push_back(instancer);
    path = instancerId;
  }

  /* The flat instance index has the outermost instancer's index as most significant part. */
  SdfPathVector paths(numInstances);
  std::vector<SdfPath> instancePaths;
  for (size_t i = 0; i < numInstances; i++) {
    size_t index = i;
    instancePaths.clear();
    for (const Instancer &instancer : instancers) {
      const int instance = instancer.indices[index % instancer.indices.size()];
      index /= instancer.indices.size();
      if (instance >= 0 && size_t(instance) < instancer.locations.size()) {
        instancePaths.push_back(instancer.locations[instance]);
      }
    }
    SdfPath &scenePath = paths[i];
    for (auto it = instancePaths.rbegin(); it != instancePaths.rend(); ++it) {
      AppendScenePath(si->GetPrim(*it).dataSource, scenePath);
    }
    AppendScenePath(primDs, scenePath);
  }
  return paths;
}

VtValue ReadPrimvar(HdPrimvarsSchema primvars, const TfToken &name)
{
  if (!primvars) {
    return {};
  }
  HdSampledDataSourceHandle ds = primvars.GetPrimvar(name).GetPrimvarValue();
  return ds ? ds->GetValue(0.0f) : VtValue();
}

HdInterpolation ReadPrimvarInterpolation(HdPrimvarsSchema primvars, const TfToken &name)
{
  if (!primvars) {
    return HdInterpolationCount;
  }
  HdTokenDataSourceHandle ds = primvars.GetPrimvar(name).GetInterpolation();
  if (!ds) {
    return HdInterpolationCount;
  }
  const TfToken token = ds->GetTypedValue(0.0f);
  if (token == HdPrimvarSchemaTokens->constant) {
    return HdInterpolationConstant;
  }
  if (token == HdPrimvarSchemaTokens->uniform) {
    return HdInterpolationUniform;
  }
  if (token == HdPrimvarSchemaTokens->varying) {
    return HdInterpolationVarying;
  }
  if (token == HdPrimvarSchemaTokens->vertex) {
    return HdInterpolationVertex;
  }
  if (token == HdPrimvarSchemaTokens->faceVarying) {
    return HdInterpolationFaceVarying;
  }
  if (token == HdPrimvarSchemaTokens->instance) {
    return HdInterpolationInstance;
  }
  return HdInterpolationCount;
}

TfToken ReadPrimvarRole(HdPrimvarsSchema primvars, const TfToken &name)
{
  if (!primvars) {
    return {};
  }
  HdTokenDataSourceHandle ds = primvars.GetPrimvar(name).GetRole();
  return ds ? ds->GetTypedValue(0.0f) : TfToken();
}

TfTokenVector PrimvarNamesAtInterpolation(HdPrimvarsSchema primvars, HdInterpolation interpolation)
{
  TfTokenVector result;
  if (!primvars) {
    return result;
  }
  for (const TfToken &name : primvars.GetPrimvarNames()) {
    if (ReadPrimvarInterpolation(primvars, name) == interpolation) {
      result.push_back(name);
    }
  }
  return result;
}

void ApplyPrimvars(AttributeSet &attributes,
                   const ustring &name,
                   VtValue value,
                   AttributeElement elem,
                   AttributeStandard std)
{
  const void *data = HdGetValueData(value);
  size_t size = value.GetArraySize();

  const HdType valueType = HdGetValueTupleType(value).type;

  TypeDesc attrType = CCL_NS::TypeUnknown;
  switch (valueType) {
    case HdTypeFloat:
      attrType = CCL_NS::TypeFloat;
      size *= sizeof(float);
      break;
    case HdTypeFloatVec2:
      attrType = CCL_NS::TypeFloat2;
      size *= sizeof(float2);
      static_assert(sizeof(GfVec2f) == sizeof(float2));
      break;
    case HdTypeFloatVec3: {
      const auto &valueData = value.Get<VtVec3fArray>();

      if (elem & ATTR_ELEMENT_IS_NORMAL) {
        attrType = CCL_NS::TypeNormal;
        size = valueData.size() * sizeof(packed_normal);

        VtArray<packed_normal> valueConverted;
        valueConverted.reserve(valueData.size());
        for (const GfVec3f &vec : valueData) {
          valueConverted.push_back(packed_normal(make_float3(vec[0], vec[1], vec[2])));
        }
        data = valueConverted.data();
        value = std::move(valueConverted);
      }
      else {
        attrType = CCL_NS::TypeVector;
        size = valueData.size() * sizeof(packed_float3);
        static_assert(sizeof(GfVec3f) == sizeof(packed_float3));
      }
      break;
    }
    case HdTypeFloatVec4:
      attrType = CCL_NS::TypeFloat4;
      size *= sizeof(float4);
      static_assert(sizeof(GfVec4f) == sizeof(float4));
      break;
    default:
      TF_WARN("Unsupported attribute type %d", static_cast<int>(valueType));
      return;
  }

  Attribute *const attr = attributes.add(name, attrType, elem);
  attr->std = std;

  assert(size == attr->data_sizeof() * attr->size);
  std::memcpy(attr->data_for_write(), data, size);
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
