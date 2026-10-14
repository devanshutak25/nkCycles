/* SPDX-FileCopyrightText: 2026 Devanshu Tak
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Hydra 2.0 schema API missing from older USD releases (Nuke 15 ships USD 23.05). */

#pragma once

#include "hydra/config.h"

#include <pxr/base/tf/staticTokens.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/cameraSchema.h>
#include <pxr/imaging/hd/materialConnectionSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/tokens.h>

#if __has_include(<pxr/imaging/hd/materialNodeParameterSchema.h>)
#  include <pxr/imaging/hd/materialNodeParameterSchema.h>
#else
PXR_NAMESPACE_OPEN_SCOPE

/* USD < 24.03: material node parameters are plain sampled data sources. */
class HdMaterialNodeParameterSchema {
 public:
  HdMaterialNodeParameterSchema(HdSampledDataSourceHandle value) : _value(std::move(value)) {}

  HdSampledDataSourceHandle GetValue() const
  {
    return _value;
  }

  explicit operator bool() const
  {
    return bool(_value);
  }

 private:
  HdSampledDataSourceHandle _value;
};

class HdMaterialNodeParameterContainerSchema {
 public:
  HdMaterialNodeParameterContainerSchema(HdContainerDataSourceHandle container)
      : _container(std::move(container))
  {
  }

  TfTokenVector GetNames() const
  {
    return _container ? _container->GetNames() : TfTokenVector();
  }

  HdMaterialNodeParameterSchema Get(const TfToken &name) const
  {
    return HdSampledDataSource::Cast(_container ? _container->Get(name) : nullptr);
  }

 private:
  HdContainerDataSourceHandle _container;
};

PXR_NAMESPACE_CLOSE_SCOPE
#endif

#if __has_include(<pxr/imaging/hd/schemaTypeDefs.h>)
#  include <pxr/imaging/hd/schemaTypeDefs.h>
#else
PXR_NAMESPACE_OPEN_SCOPE

/* USD < 24.03: typed container and vector schemas over plain data sources. */
template<typename Schema> class HdCyclesVectorSchema {
 public:
  HdCyclesVectorSchema(HdVectorDataSourceHandle vector) : _vector(std::move(vector)) {}

  size_t GetNumElements() const
  {
    return _vector ? _vector->GetNumElements() : 0;
  }

  Schema GetElement(const size_t i) const
  {
    return Schema(_vector ? HdContainerDataSource::Cast(_vector->GetElement(i)) : nullptr);
  }

 private:
  HdVectorDataSourceHandle _vector;
};

template<typename Schema, typename DataSource = HdContainerDataSource>
class HdCyclesContainerSchema {
 public:
  HdCyclesContainerSchema(HdContainerDataSourceHandle container) : _container(std::move(container))
  {
  }

  TfTokenVector GetNames() const
  {
    return _container ? _container->GetNames() : TfTokenVector();
  }

  Schema Get(const TfToken &name) const
  {
    return Schema(_container ? DataSource::Cast(_container->Get(name)) : nullptr);
  }

 private:
  HdContainerDataSourceHandle _container;
};

using HdMaterialNodeContainerSchema = HdCyclesContainerSchema<HdMaterialNodeSchema>;
using HdMaterialConnectionContainerSchema = HdCyclesContainerSchema<HdMaterialConnectionSchema>;
using HdMaterialConnectionVectorSchema = HdCyclesVectorSchema<HdMaterialConnectionSchema>;
using HdMaterialConnectionVectorContainerSchema =
    HdCyclesContainerSchema<HdMaterialConnectionVectorSchema, HdVectorDataSource>;

PXR_NAMESPACE_CLOSE_SCOPE
#endif

#if __has_include(<pxr/imaging/hd/materialBindingsSchema.h>)
#  include <pxr/imaging/hd/materialBindingsSchema.h>
#else
#  include <pxr/imaging/hd/materialBindingSchema.h>
PXR_NAMESPACE_OPEN_SCOPE

/* USD < 23.08: a single material binding schema returning the path directly. */
class HdMaterialBindingsSchema {
 public:
  struct Binding {
    HdPathDataSourceHandle path;
    HdPathDataSourceHandle GetPath() const
    {
      return path;
    }
  };

  HdMaterialBindingsSchema(HdMaterialBindingSchema schema) : _schema(schema) {}

  static HdMaterialBindingsSchema GetFromParent(const HdContainerDataSourceHandle &parent)
  {
    return HdMaterialBindingSchema::GetFromParent(parent);
  }

  Binding GetMaterialBinding()
  {
    return {_schema.GetMaterialBinding()};
  }

 private:
  HdMaterialBindingSchema _schema;
};

PXR_NAMESPACE_CLOSE_SCOPE
#endif

HDCYCLES_NAMESPACE_OPEN_SCOPE

#if PXR_VERSION < 2308
/* USD < 23.08: no camera schema accessors for depth of field and shutter, the camera
 * container still provides them by name. */
class HdCyclesCameraSchema : public HdCameraSchema {
 public:
  HdCyclesCameraSchema(HdCameraSchema schema) : HdCameraSchema(schema) {}

  HdFloatDataSourceHandle GetFStop()
  {
    return GetFloat(HdCameraTokens->fStop);
  }
  HdFloatDataSourceHandle GetFocusDistance()
  {
    return GetFloat(HdCameraTokens->focusDistance);
  }
  HdFloatDataSourceHandle GetShutterOpen()
  {
    return GetFloat(HdCameraTokens->shutterOpen);
  }
  HdFloatDataSourceHandle GetShutterClose()
  {
    return GetFloat(HdCameraTokens->shutterClose);
  }

 private:
  HdFloatDataSourceHandle GetFloat(const TfToken &name)
  {
    const HdContainerDataSourceHandle container = GetContainer();
    const HdSampledDataSourceHandle ds = HdSampledDataSource::Cast(
        container ? container->Get(name) : nullptr);
    if (!ds) {
      return nullptr;
    }
    const VtValue value = ds->GetValue(0.0f);
    if (!value.CanCast<float>()) {
      return nullptr;
    }
    return HdRetainedTypedSampledDataSource<float>::New(VtValue::Cast<float>(value).Get<float>());
  }
};
#else
using HdCyclesCameraSchema = HdCameraSchema;
#endif

#if PXR_VERSION < 2405
/* USD 23.05 does not export HdMaterialSchema::GetMaterialNetwork (no HD_API). */
class HdCyclesMaterialSchema : public HdMaterialSchema {
 public:
  HdCyclesMaterialSchema(HdMaterialSchema schema) : HdMaterialSchema(schema) {}

  HdContainerDataSourceHandle GetMaterialNetwork()
  {
    return GetNetwork(HdMaterialSchemaTokens->universalRenderContext);
  }
  HdContainerDataSourceHandle GetMaterialNetwork(const TfToken &context)
  {
    if (HdContainerDataSourceHandle network = GetNetwork(context)) {
      return network;
    }
    return GetMaterialNetwork();
  }

 private:
  HdContainerDataSourceHandle GetNetwork(const TfToken &context)
  {
    const HdContainerDataSourceHandle container = GetContainer();
    return container ? HdContainerDataSource::Cast(container->Get(context)) : nullptr;
  }
};
#else
using HdCyclesMaterialSchema = HdMaterialSchema;
#endif

#if PXR_VERSION < 2311
/* USD < 23.11 instancer primvar names. */
TF_DEFINE_PRIVATE_TOKENS(HdCyclesInstancerTokens,
                         ((instanceTranslations, "translate"))((instanceRotations, "rotate"))((
                             instanceScales, "scale"))((instanceTransforms, "instanceTransform")));
#else
#  define HdCyclesInstancerTokens HdInstancerTokens
#endif

HDCYCLES_NAMESPACE_CLOSE_SCOPE
