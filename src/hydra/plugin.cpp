/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/plugin.h"
#include "hydra/render_delegate.h"
#include "util/log.h"
#include "util/path.h"

#include <pxr/base/arch/fileSystem.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/thisPlugin.h>
#include <pxr/base/tf/envSetting.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#if PXR_VERSION >= 2408
#  include <pxr/imaging/hd/retainedDataSource.h>
#  include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#  include <pxr/imaging/hd/tokens.h>
#  include <pxr/imaging/hdsi/implicitSurfaceSceneIndex.h>
#endif

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_ENV_SETTING(CYCLES_LOGGING, false, "Enable Cycles logging")
TF_DEFINE_ENV_SETTING(CYCLES_LOGGING_LEVEL, "warning", "Cycles logging level")

HdCyclesPlugin::HdCyclesPlugin()
{
  const PlugPluginPtr plugin = PLUG_THIS_PLUGIN;
  // Initialize Cycles paths relative to the plugin resource path
  const std::string rootPath = PXR_NS::ArchAbsPath(plugin->GetResourcePath());
  CCL_NS::path_init(std::move(rootPath));

  if (TfGetEnvSetting(CYCLES_LOGGING)) {
    CCL_NS::log_level_set(TfGetEnvSetting(CYCLES_LOGGING_LEVEL));
  }
}

HdCyclesPlugin::~HdCyclesPlugin() {}

#if PXR_VERSION < 2302
bool HdCyclesPlugin::IsSupported() const
{
  return true;
}
#else
#  if PXR_VERSION >= 2511
bool HdCyclesPlugin::IsSupported(HdRendererCreateArgs const & /*rendererCreateArgs*/,
                                 std::string * /*reasonWhyNot*/) const
{
  return true;
}
#  endif
bool HdCyclesPlugin::IsSupported(bool /*gpuEnabled*/) const
{
  return true;
}
#endif

HdRenderDelegate *HdCyclesPlugin::CreateRenderDelegate()
{
  return CreateRenderDelegate({});
}

HdRenderDelegate *HdCyclesPlugin::CreateRenderDelegate(const HdRenderSettingsMap &settingsMap)
{
  return new HD_CYCLES_NS::HdCyclesDelegate(settingsMap);
}

void HdCyclesPlugin::DeleteRenderDelegate(HdRenderDelegate *renderDelegate)
{
  delete renderDelegate;
}

// USD's type system accounts for namespace, so we'd have to register our name as
// HdCycles::HdCyclesPlugin in plugInfo.json, which isn't all that bad for JSON,
// but those colons may cause issues for any USD specific tooling. So just put our
// plugin class in the pxr namespace (which USD's type system will elide).
TF_REGISTRY_FUNCTION(TfType)
{
  HdRendererPluginRegistry::Define<PXR_NS::HdCyclesPlugin>();
#if PXR_VERSION >= 2408
  HdSceneIndexPluginRegistry::Define<PXR_NS::HdCycles_ImplicitSurfaceSceneIndexPlugin>();
#endif
}

#if PXR_VERSION >= 2408
HdSceneIndexBaseRefPtr HdCycles_ImplicitSurfaceSceneIndexPlugin::_AppendSceneIndex(
    const HdSceneIndexBaseRefPtr &inputScene, const HdContainerDataSourceHandle &inputArgs)
{
  return HdsiImplicitSurfaceSceneIndex::New(inputScene, inputArgs);
}

// Cycles has no implicit surfaces, so turn USD Sphere, Cube, Cone, Cylinder, Capsule and Plane
// into meshes before they reach the render delegate.
TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
  const HdDataSourceBaseHandle toMesh = HdRetainedTypedSampledDataSource<TfToken>::New(
      HdsiImplicitSurfaceSceneIndexTokens->toMesh);
  const TfToken names[] = {HdPrimTypeTokens->sphere,
                           HdPrimTypeTokens->cube,
                           HdPrimTypeTokens->cone,
                           HdPrimTypeTokens->cylinder,
                           HdPrimTypeTokens->capsule,
                           HdPrimTypeTokens->plane};
  const HdDataSourceBaseHandle values[] = {toMesh, toMesh, toMesh, toMesh, toMesh, toMesh};

  HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
      "Cycles",
      TfToken("HdCycles_ImplicitSurfaceSceneIndexPlugin"),
      HdRetainedContainerDataSource::New(std::size(names), names, values),
      0,
      HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}
#endif

PXR_NAMESPACE_CLOSE_SCOPE
