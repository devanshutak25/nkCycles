/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/render_delegate.h"
#include "hydra/camera.h"
#include "hydra/curves.h"
#include "hydra/field.h"
#include "hydra/instancer.h"
#include "hydra/light.h"
#include "hydra/material.h"
#ifdef WITH_HYDRA_MATERIALX
#  include "hydra/materialx.h"
#endif
#include "hydra/mesh.h"
#include "hydra/node_util.h"
#include "hydra/pointcloud.h"
#include "hydra/render_buffer.h"
#include "hydra/render_pass.h"
#include "hydra/session.h"
#include "hydra/volume.h"
#include "scene/background.h"
#include "scene/film.h"
#include "scene/integrator.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "util/murmurhash.h"
#include "util/string.h"
#include "session/session.h"

#include <array>

#include <pxr/base/tf/getenv.h>
#include <pxr/imaging/hgi/tokens.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PUBLIC_TOKENS(HdCyclesRenderSettingsTokens, HD_CYCLES_RENDER_SETTINGS_TOKENS);

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
    (cycles)
    (openvdbAsset)
    (houdiniFieldAsset)
);
// clang-format on

namespace {

const TfTokenVector kSupportedRPrimTypes = {
    HdPrimTypeTokens->basisCurves,
    HdPrimTypeTokens->mesh,
    HdPrimTypeTokens->points,
#ifdef WITH_OPENVDB
    HdPrimTypeTokens->volume,
#endif
};

const TfTokenVector kSupportedSPrimTypes = {
    HdPrimTypeTokens->camera,
    HdPrimTypeTokens->material,
    HdPrimTypeTokens->diskLight,
    HdPrimTypeTokens->distantLight,
    HdPrimTypeTokens->domeLight,
    HdPrimTypeTokens->rectLight,
    HdPrimTypeTokens->sphereLight,
};

const TfTokenVector kSupportedBPrimTypes = {
    HdPrimTypeTokens->renderBuffer,
#ifdef WITH_OPENVDB
    _tokens->openvdbAsset,
#endif
#ifdef WITH_HOUDINI_VOLUMES
    _tokens->houdiniFieldAsset,
#endif
};

// Houdini sends menu values as tokens, while other clients may use strings
std::string GetStringSetting(const VtValue &value, const std::string &defaultValue)
{
  if (value.IsHolding<TfToken>()) {
    return value.UncheckedGet<TfToken>().GetString();
  }
  return VtValue::Cast<std::string>(value).GetWithDefault(defaultValue);
}

// Scene nodes whose sockets are exposed as "cycles:<node>:<socket>" render settings
std::array<std::pair<std::string, Node *>, 3> GetSettingNodes(Scene *scene)
{
  return {{
      {"cycles:integrator:", scene->integrator},
      {"cycles:film:", scene->film},
      {"cycles:background:", scene->background},
  }};
}

Node *FindSettingNode(Scene *scene, const std::string &key, ustring &socketName)
{
  for (const auto &[prefix, node] : GetSettingNodes(scene)) {
    if (key.compare(0, prefix.size(), prefix) == 0) {
      socketName = ustring(key.c_str() + prefix.size());
      return node;
    }
  }
  return nullptr;
}

}  // namespace

SessionParams GetSessionParams(const HdRenderSettingsMap &settings)
{
  SessionParams params;
  params.threads = 0;
  params.background = false;
  params.use_resolution_divider = false;

  HdRenderSettingsMap::const_iterator it;

  // Pull all setting that contribute to device creation first
  it = settings.find(HdCyclesRenderSettingsTokens->threads);
  if (it != settings.end()) {
    params.threads = VtValue::Cast<int>(it->second).GetWithDefault(params.threads);
  }

  // Get the Cycles device from settings or environment, defaulting to the best available GPU
  std::string deviceType = "AUTO";
  it = settings.find(HdCyclesRenderSettingsTokens->device);
  if (it != settings.end()) {
    deviceType = GetStringSetting(it->second, deviceType);
  }
  else {
    const std::string deviceTypeEnv = TfGetenv("CYCLES_DEVICE");
    if (!deviceTypeEnv.empty()) {
      deviceType = deviceTypeEnv;
    }
  }

  // Move to all uppercase for Device::type_from_string
  std::transform(deviceType.begin(), deviceType.end(), deviceType.begin(), ::toupper);

  vector<DeviceInfo> devices;
  if (deviceType == "AUTO") {
    // Prefer OptiX over CUDA on NVIDIA, like the order of the Blender preferences
    for (const DeviceType type :
         {DEVICE_OPTIX, DEVICE_CUDA, DEVICE_HIP, DEVICE_METAL, DEVICE_ONEAPI})
    {
      for (const DeviceInfo &info : Device::available_devices(DEVICE_MASK(type))) {
        if (info.meets_driver_requirement) {
          devices.push_back(info);
        }
      }
      if (!devices.empty()) {
        break;
      }
    }
  }
  else {
    for (const DeviceInfo &info :
         Device::available_devices(DEVICE_MASK(Device::type_from_string(deviceType.c_str()))))
    {
      if (info.meets_driver_requirement) {
        devices.push_back(info);
      }
    }
  }
  if (devices.empty()) {
    devices = Device::available_devices(DEVICE_MASK_CPU);
    if (!devices.empty()) {
      params.device = devices.front();
    }
  }
  else {
    params.device = Device::get_multi_device(devices, params.threads, params.background);
  }

  /* Set same device for denoising for now. */
  params.denoise_device = params.device;

  return params;
}

HdCyclesDelegate::HdCyclesDelegate(const HdRenderSettingsMap &settingsMap,
                                   Session *session_,
                                   const bool keep_nodes)
    : HdRenderDelegate()
{
  _renderParam = session_ ? std::make_unique<HdCyclesSession>(session_, keep_nodes) :
                            std::make_unique<HdCyclesSession>(GetSessionParams(settingsMap));

  for (const auto &setting : settingsMap) {
    // Skip over the settings known to be used for initialization only
    if (setting.first == HdCyclesRenderSettingsTokens->device ||
        setting.first == HdCyclesRenderSettingsTokens->threads)
    {
      continue;
    }

    SetRenderSetting(setting.first, setting.second);
  }
}

HdCyclesDelegate::~HdCyclesDelegate() = default;

void HdCyclesDelegate::SetDrivers(const HdDriverVector &drivers)
{
  for (HdDriver *hdDriver : drivers) {
    if (hdDriver->name == HgiTokens->renderDriver && hdDriver->driver.IsHolding<Hgi *>()) {
      _hgi = hdDriver->driver.UncheckedGet<Hgi *>();
      break;
    }
  }
}

bool HdCyclesDelegate::IsDisplaySupported() const
{
#if defined(_WIN32) && defined(WITH_HYDRA_DISPLAY_DRIVER)
  return _hgi && _hgi->GetAPIName() == HgiTokens->OpenGL;
#else
  return false;
#endif
}

const TfTokenVector &HdCyclesDelegate::GetSupportedRprimTypes() const
{
  return kSupportedRPrimTypes;
}

const TfTokenVector &HdCyclesDelegate::GetSupportedSprimTypes() const
{
  return kSupportedSPrimTypes;
}

const TfTokenVector &HdCyclesDelegate::GetSupportedBprimTypes() const
{
  return kSupportedBPrimTypes;
}

HdRenderParam *HdCyclesDelegate::GetRenderParam() const
{
  return _renderParam.get();
}

HdResourceRegistrySharedPtr HdCyclesDelegate::GetResourceRegistry() const
{
  return HdResourceRegistrySharedPtr();
}

bool HdCyclesDelegate::IsPauseSupported() const
{
  return true;
}

bool HdCyclesDelegate::Pause()
{
  _renderParam->session->set_pause(true);
  return true;
}

bool HdCyclesDelegate::Resume()
{
  _renderParam->session->set_pause(false);
  return true;
}

HdRenderPassSharedPtr HdCyclesDelegate::CreateRenderPass(HdRenderIndex *index,
                                                         const HdRprimCollection &collection)
{
  return HdRenderPassSharedPtr(new HdCyclesRenderPass(index, collection, _renderParam.get()));
}

HdInstancer *HdCyclesDelegate::CreateInstancer(HdSceneDelegate *delegate,
                                               const SdfPath &instancerId)
{
  return new HdCyclesInstancer(delegate, instancerId);
}

void HdCyclesDelegate::DestroyInstancer(HdInstancer *instancer)
{
  delete instancer;
}

HdRprim *HdCyclesDelegate::CreateRprim(const TfToken &typeId, const SdfPath &rprimId)
{
  if (typeId == HdPrimTypeTokens->mesh) {
    return new HdCyclesMesh(rprimId);
  }
  if (typeId == HdPrimTypeTokens->basisCurves) {
    return new HdCyclesCurves(rprimId);
  }
  if (typeId == HdPrimTypeTokens->points) {
    return new HdCyclesPoints(rprimId);
  }
#ifdef WITH_OPENVDB
  if (typeId == HdPrimTypeTokens->volume) {
    return new HdCyclesVolume(rprimId);
  }
#endif

  TF_CODING_ERROR("Unknown Rprim type %s", typeId.GetText());
  return nullptr;
}

void HdCyclesDelegate::DestroyRprim(HdRprim *rPrim)
{
  delete rPrim;
}

HdSprim *HdCyclesDelegate::CreateSprim(const TfToken &typeId, const SdfPath &sprimId)
{
  if (typeId == HdPrimTypeTokens->camera) {
    return new HdCyclesCamera(sprimId);
  }
  if (typeId == HdPrimTypeTokens->material) {
    return new HdCyclesMaterial(sprimId);
  }
  if (typeId == HdPrimTypeTokens->diskLight || typeId == HdPrimTypeTokens->distantLight ||
      typeId == HdPrimTypeTokens->domeLight || typeId == HdPrimTypeTokens->rectLight ||
      typeId == HdPrimTypeTokens->sphereLight)
  {
    return new HdCyclesLight(sprimId, typeId);
  }

  TF_CODING_ERROR("Unknown Sprim type %s", typeId.GetText());
  return nullptr;
}

HdSprim *HdCyclesDelegate::CreateFallbackSprim(const TfToken &typeId)
{
  return CreateSprim(typeId, SdfPath::EmptyPath());
}

void HdCyclesDelegate::DestroySprim(HdSprim *sPrim)
{
  delete sPrim;
}

HdBprim *HdCyclesDelegate::CreateBprim(const TfToken &typeId, const SdfPath &bprimId)
{
  if (typeId == HdPrimTypeTokens->renderBuffer) {
    return new HdCyclesRenderBuffer(bprimId);
  }
#ifdef WITH_OPENVDB
  if (typeId == _tokens->openvdbAsset) {
    return new HdCyclesField(bprimId, typeId);
  }
#endif
#ifdef WITH_HOUDINI_VOLUMES
  if (typeId == _tokens->houdiniFieldAsset) {
    return new HdCyclesField(bprimId, typeId);
  }
#endif

  TF_CODING_ERROR("Unknown Bprim type %s", typeId.GetText());
  return nullptr;
}

HdBprim *HdCyclesDelegate::CreateFallbackBprim(const TfToken &typeId)
{
  return CreateBprim(typeId, SdfPath::EmptyPath());
}

void HdCyclesDelegate::DestroyBprim(HdBprim *bPrim)
{
  delete bPrim;
}

void HdCyclesDelegate::CommitResources(HdChangeTracker *tracker)
{
  TF_UNUSED(tracker);

  const SceneLock lock(_renderParam.get());

  _renderParam->UpdateScene();
}

TfToken HdCyclesDelegate::GetMaterialBindingPurpose() const
{
  return HdTokens->full;
}

TfTokenVector HdCyclesDelegate::GetMaterialRenderContexts() const
{
  TfTokenVector contexts = {_tokens->cycles};
#ifdef WITH_HYDRA_MATERIALX
  contexts.insert(contexts.end(), MaterialXRenderContexts().begin(), MaterialXRenderContexts().end());
#endif
  return contexts;
}

VtDictionary HdCyclesDelegate::GetRenderStats() const
{
  const Stats &stats = _renderParam->session->stats;
  const Progress &progress = _renderParam->session->progress;

  double totalTime;
  double renderTime;
  progress.get_time(totalTime, renderTime);
  const double fractionDone = progress.get_progress();

  std::string status;
  std::string substatus;
  progress.get_status(status, substatus);
  if (!substatus.empty()) {
    status += " | " + substatus;
  }

  VtDictionary renderStats = {{"rendererName", VtValue("Cycles")},
                              {"rendererVersion", VtValue(GfVec3i(0, 0, 0))},
                              {"percentDone", VtValue(floor_to_int(fractionDone * 100))},
                              {"fractionDone", VtValue(fractionDone)},
                              {"loadClockTime", VtValue(totalTime - renderTime)},
                              {"peakMemory", VtValue(stats.mem_peak)},
                              {"totalClockTime", VtValue(totalTime)},
                              {"totalMemory", VtValue(stats.mem_used)},
                              {"renderProgressAnnotation", VtValue(status)}};

  // Cryptomatte metadata of the specification, which husk writes into the image with the stats
  Scene *const scene = _renderParam->session->scene.get();
  const int cryptomatte = scene->film->get_cryptomatte_passes();
  if (cryptomatte != CRYPT_NONE) {
    const thread_scoped_lock lock(scene->mutex);
    auto addLayer = [&](const string &name, const string &manifest) {
      const uint32_t hash = util_murmur_hash3(name.c_str(), name.length(), 0);
      const string prefix = string_printf("cryptomatte/%08x", hash).substr(0, 19) + "/";
      renderStats[prefix + "name"] = VtValue(name);
      renderStats[prefix + "hash"] = VtValue(string("MurmurHash3_32"));
      renderStats[prefix + "conversion"] = VtValue(string("uint32_to_float32"));
      renderStats[prefix + "manifest"] = VtValue(manifest);
    };
    if (cryptomatte & CRYPT_OBJECT) {
      addLayer("CryptoObject", scene->object_manager->get_cryptomatte_objects(scene));
    }
    if (cryptomatte & CRYPT_MATERIAL) {
      addLayer("CryptoMaterial", scene->shader_manager->get_cryptomatte_materials(scene));
    }
    if (cryptomatte & CRYPT_ASSET) {
      addLayer("CryptoAsset", scene->object_manager->get_cryptomatte_assets(scene));
    }
  }

  return renderStats;
}

HdAovDescriptor HdCyclesDelegate::GetDefaultAovDescriptor(const TfToken &name) const
{
  if (name == HdAovTokens->color) {
    HdFormat colorFormat = HdFormatFloat32Vec4;
    if (IsDisplaySupported()) {
      // Can use Cycles 'DisplayDriver' in OpenGL, but it only supports 'half4' format
      colorFormat = HdFormatFloat16Vec4;
    }

    return HdAovDescriptor(colorFormat, false, VtValue(GfVec4f(0.0f)));
  }
  if (name == HdAovTokens->depth) {
    return HdAovDescriptor(HdFormatFloat32, false, VtValue(1.0f));
  }
  if (name == HdAovTokens->normal) {
    return HdAovDescriptor(HdFormatFloat32Vec3, false, VtValue(GfVec3f(0.0f)));
  }
  if (name == HdAovTokens->primId || name == HdAovTokens->instanceId ||
      name == HdAovTokens->elementId)
  {
    return HdAovDescriptor(HdFormatInt32, false, VtValue(-1));
  }

  const PassType type = HdCyclesSession::GetPassType(name);
  if (type != PASS_NONE) {
    switch (Pass::get_info(type).num_components) {
      case 1:
        return HdAovDescriptor(HdFormatFloat32, false, VtValue(0.0f));
      case 3:
        return HdAovDescriptor(HdFormatFloat32Vec3, false, VtValue(GfVec3f(0.0f)));
      case 4:
        return HdAovDescriptor(HdFormatFloat32Vec4, false, VtValue(GfVec4f(0.0f)));
      default:
        break;
    }
  }

  return HdAovDescriptor();
}

HdRenderSettingDescriptorList HdCyclesDelegate::GetRenderSettingDescriptors() const
{
  Scene *const scene = _renderParam->session->scene.get();

  HdRenderSettingDescriptorList descriptors;

  descriptors.push_back({
      "Time Limit",
      HdCyclesRenderSettingsTokens->timeLimit,
      VtValue(0.0),
  });
  descriptors.push_back({
      "Sample Count",
      HdCyclesRenderSettingsTokens->samples,
      VtValue(1024),
  });
  descriptors.push_back({
      "Sample Offset",
      HdCyclesRenderSettingsTokens->sampleOffset,
      VtValue(0),
  });

  for (const auto &[prefix, node] : GetSettingNodes(scene)) {
    for (const SocketType &socket : node->type->inputs) {
      if (socket.type == SocketType::NODE) {
        continue;
      }
      descriptors.push_back({socket.ui_name.string(),
                             TfToken(prefix + socket.name.string()),
                             GetNodeValue(node, socket)});
    }
  }

  return descriptors;
}

void HdCyclesDelegate::SetRenderSetting(const PXR_NS::TfToken &key, const PXR_NS::VtValue &value)
{
  Scene *const scene = _renderParam->session->scene.get();
  Session *const session = _renderParam->session;

  if (key == HdCyclesRenderSettingsTokens->stageMetersPerUnit) {
    _renderParam->SetStageMetersPerUnit(
        VtValue::Cast<double>(value).GetWithDefault(_renderParam->GetStageMetersPerUnit()));
  }
  else if (key == HdCyclesRenderSettingsTokens->timeLimit) {
    session->set_time_limit(
        VtValue::Cast<double>(value).GetWithDefault(session->params.time_limit));
  }
  else if (key == HdCyclesRenderSettingsTokens->samples) {
    static const int max_samples = Integrator::MAX_SAMPLES;
    int samples = VtValue::Cast<int>(value).GetWithDefault(session->params.samples);
    samples = std::min(std::max(1, samples), max_samples);
    session->set_samples(samples);
  }
  else if (key == HdCyclesRenderSettingsTokens->sampleOffset) {
    session->params.sample_subset_offset = VtValue::Cast<int>(value).GetWithDefault(
        session->params.sample_subset_offset);
    session->params.sample_subset_length = Integrator::MAX_SAMPLES;
    session->params.use_sample_subset = session->params.sample_subset_offset > 0;
    ++_settingsVersion;
  }
  else if (key == HdCyclesRenderSettingsTokens->houdiniViewport) {
    _renderParam->SetDepthToWindow(VtValue::Cast<bool>(value).GetWithDefault(false));
  }
  else if (key == HdCyclesRenderSettingsTokens->transparentBackground) {
    _renderParam->SetTransparentBackground(
        value.IsEmpty() ? std::nullopt :
                          std::optional<bool>(VtValue::Cast<bool>(value).GetWithDefault(false)));
    ++_settingsVersion;
  }
  else {
    ustring socketName;
    if (Node *const node = FindSettingNode(scene, key.GetString(), socketName)) {
      if (const SocketType *socket = node->type->find_input(socketName)) {
        SetNodeValue(node, *socket, value);
        ++_settingsVersion;

        /* The session overwrites the integrator sample subset with its own on every reset. */
        if (node == scene->integrator &&
            (socketName == "use_sample_subset" || socketName == "sample_subset_offset" ||
             socketName == "sample_subset_length"))
        {
          session->params.use_sample_subset = scene->integrator->get_use_sample_subset();
          session->params.sample_subset_offset = scene->integrator->get_sample_subset_offset();
          session->params.sample_subset_length = scene->integrator->get_sample_subset_length();
        }
      }
    }
  }
}

VtValue HdCyclesDelegate::GetRenderSetting(const TfToken &key) const
{
  Scene *const scene = _renderParam->session->scene.get();
  Session *const session = _renderParam->session;

  if (key == HdCyclesRenderSettingsTokens->stageMetersPerUnit) {
    return VtValue(_renderParam->GetStageMetersPerUnit());
  }
  if (key == HdCyclesRenderSettingsTokens->device) {
    return VtValue(TfToken(Device::string_from_type(session->params.device.type)));
  }
  if (key == HdCyclesRenderSettingsTokens->threads) {
    return VtValue(session->params.threads);
  }
  if (key == HdCyclesRenderSettingsTokens->timeLimit) {
    return VtValue(session->params.time_limit);
  }
  if (key == HdCyclesRenderSettingsTokens->samples) {
    return VtValue(session->params.samples);
  }
  if (key == HdCyclesRenderSettingsTokens->sampleOffset) {
    return VtValue((session->params.use_sample_subset) ? session->params.sample_subset_offset : 0);
  }

  ustring socketName;
  if (Node *const node = FindSettingNode(scene, key.GetString(), socketName)) {
    if (const SocketType *socket = node->type->find_input(socketName)) {
      return GetNodeValue(node, *socket);
    }
  }

  return VtValue();
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
