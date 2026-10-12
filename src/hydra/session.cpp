/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/session.h"
#include "scene/object.h"
#include "scene/shader.h"
// Have to include shader.h before background.h so that 'set_shader' uses the correct 'set'
// overload taking a 'Node *', rather than the one taking a 'bool'
#include "scene/background.h"
#include "scene/film.h"
#include "scene/light.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "session/session.h"
#include "util/string.h"

#include <pxr/imaging/hd/renderBuffer.h>

#include <algorithm>
#include <cctype>
#include <cstring>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

const std::unordered_map<TfToken, PassType, TfToken::HashFunctor> kAovToPass = {
    {HdAovTokens->color, PASS_COMBINED},
    {HdAovTokens->depth, PASS_DEPTH},
    {HdAovTokens->normal, PASS_NORMAL},
    {HdAovTokens->primId, PASS_OBJECT_ID},
    {HdAovTokens->instanceId, PASS_AOV_VALUE},
};

// Light path expressions of Karma's AOVs, matched to the Cycles light passes holding the same
// light paths. These passes include the albedo, like an LPE.
const std::unordered_map<string, PassType> kLpeToPass = {
    {"C.*[LO]", PASS_COMBINED},
    {"C<RD>.*L", PASS_DIFFUSE},
    {"C<RD>L", PASS_DIFFUSE_DIRECT},
    {"C<RD>.+L", PASS_DIFFUSE_INDIRECT},
    {"C<RG>.*L", PASS_GLOSSY},
    {"C<RG>L", PASS_GLOSSY_DIRECT},
    {"C<RG>.+L", PASS_GLOSSY_INDIRECT},
    {"C<TG>.*L", PASS_TRANSMISSION},
    {"C<TG>L", PASS_TRANSMISSION_DIRECT},
    {"C<TG>.+L", PASS_TRANSMISSION_INDIRECT},
    {"CV.*L", PASS_VOLUME},
    {"CVL", PASS_VOLUME_DIRECT},
    {"CV.+L", PASS_VOLUME_INDIRECT},
    {"CO", PASS_EMISSION},
    {"CDA", PASS_DIFFUSE_COLOR},
    {"C[DV]A", PASS_DIFFUSE_COLOR},
};

PassType LpeToPassType(string lpe)
{
  lpe.erase(std::remove(lpe.begin(), lpe.end(), ' '), lpe.end());
  const auto it = kLpeToPass.find(lpe);
  return it != kLpeToPass.end() ? it->second : PASS_NONE;
}

string GetAovSetting(const HdRenderPassAovBinding &aovBinding, const TfToken &key)
{
  const auto it = aovBinding.aovSettings.find(key);
  if (it == aovBinding.aovSettings.end()) {
    return string();
  }
  if (it->second.IsHolding<TfToken>()) {
    return it->second.UncheckedGet<TfToken>().GetString();
  }
  return it->second.IsHolding<string>() ? it->second.UncheckedGet<string>() : string();
}

// Light path expression of a render var: husk passes it in the AOV settings, others as an
// "lpe:" AOV name.
string GetLpe(const HdRenderPassAovBinding &aovBinding)
{
  if (GetAovSetting(aovBinding, TfToken("sourceType")) == "lpe") {
    return GetAovSetting(aovBinding, TfToken("sourceName"));
  }
  const string &name = aovBinding.aovName.GetString();
  return TfStringStartsWith(name, "lpe:") ? name.substr(4) : string();
}

// Source name of a render var: husk names AOVs after the render var prim and passes the source
// name in the AOV settings.
TfToken GetSourceName(const HdRenderPassAovBinding &aovBinding)
{
  const string sourceName = GetAovSetting(aovBinding, TfToken("sourceName"));
  return sourceName.empty() ? aovBinding.aovName : TfToken(sourceName);
}

// Cryptomatte render vars have Blender's layer names, "CryptoObject00", "CryptoMaterial01", ...
// with two ranks per layer.
const std::pair<const char *, CryptomatteType> kCryptomatteTypes[] = {
    {"CryptoObject", CRYPT_OBJECT},
    {"CryptoMaterial", CRYPT_MATERIAL},
    {"CryptoAsset", CRYPT_ASSET},
};

bool ParseCryptomatte(const string &name, CryptomatteType &type, int &index)
{
  for (const auto &[prefix, cryptomatteType] : kCryptomatteTypes) {
    const size_t length = strlen(prefix);
    if (name.size() == length + 2 && TfStringStartsWith(name, prefix) && isdigit(name[length]) &&
        isdigit(name[length + 1]))
    {
      type = cryptomatteType;
      index = std::stoi(name.substr(length));
      return true;
    }
  }
  return false;
}

// Light path expressions, Hydra AOVs, then Cycles pass names ("diffuse_direct", "mist", ...),
// then shader AOVs written by the AOV Output node, matched by name.
PassType AovToPassType(const HdRenderPassAovBinding &aovBinding)
{
  const string lpe = GetLpe(aovBinding);
  if (!lpe.empty()) {
    return LpeToPassType(lpe);
  }

  const TfToken sourceName = GetSourceName(aovBinding);
  const PassType type = HdCyclesSession::GetPassType(sourceName);
  if (type != PASS_NONE) {
    return type;
  }

  const string &name = sourceName.GetString();

  // Primvars are not supported
  if (TfStringStartsWith(name, "primvars:")) {
    return PASS_NONE;
  }

  const HdFormat format = aovBinding.renderBuffer ? aovBinding.renderBuffer->GetFormat() :
                                                    HdFormatInvalid;
  return HdGetComponentCount(format) == 1 ? PASS_AOV_VALUE : PASS_AOV_COLOR;
}

}  // namespace

PassType HdCyclesSession::GetPassType(const TfToken &aovName)
{
  const auto hydraAov = kAovToPass.find(aovName);
  if (hydraAov != kAovToPass.end()) {
    return hydraAov->second;
  }
  if (TfStringStartsWith(aovName.GetString(), "lpe:")) {
    return LpeToPassType(aovName.GetString().substr(4));
  }
  const NodeEnum *passTypes = Pass::get_type_enum();
  if (passTypes->exists(ustring(aovName.GetString()))) {
    const PassType type = PassType((*passTypes)[aovName.GetText()]);
    // Shader AOV passes need a name of their own, and cryptomatte needs film settings
    if (type != PASS_AOV_COLOR && type != PASS_AOV_VALUE && type != PASS_CRYPTOMATTE) {
      return type;
    }
  }
  return PASS_NONE;
}

SceneLock::SceneLock(const HdRenderParam *renderParam)
    : scene(static_cast<const HdCyclesSession *>(renderParam)->session->scene.get()),
      sceneLock(scene->mutex)
{
}

SceneLock::~SceneLock() = default;

HdCyclesSession::HdCyclesSession(Session *session_, const bool keep_nodes)
    : session(session_), keep_nodes(keep_nodes), _ownCyclesSession(false)
{
}

HdCyclesSession::HdCyclesSession(const SessionParams &params)
    : session(new Session(params, SceneParams())), keep_nodes(false), _ownCyclesSession(true)
{
  Scene *const scene = session->scene.get();

  // Create background with ambient light
  {
    unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();

    BackgroundNode *bgNode = graph->create_node<BackgroundNode>();
    bgNode->set_color(one_float3());

    graph->connect(bgNode->output("Background"), graph->output()->input("Surface"));

    scene->default_background->set_graph(std::move(graph));
    scene->default_background->tag_update(scene);
  }

  // Wire up object color in default surface material
  {
    unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();

    ObjectInfoNode *objectNode = graph->create_node<ObjectInfoNode>();

    DiffuseBsdfNode *diffuseNode = graph->create_node<DiffuseBsdfNode>();

    graph->connect(objectNode->output("Color"), diffuseNode->input("Color"));
    graph->connect(diffuseNode->output("BSDF"), graph->output()->input("Surface"));

    // Create the instanceId AOV output
    const ustring instanceId(HdAovTokens->instanceId.GetString());

    OutputAOVNode *aovNode = graph->create_node<OutputAOVNode>();
    aovNode->set_name(instanceId);

    AttributeNode *instanceIdNode = graph->create_node<AttributeNode>();
    instanceIdNode->set_attribute(instanceId);

    graph->connect(instanceIdNode->output("Fac"), aovNode->input("Value"));

    scene->default_surface->set_graph(std::move(graph));
    scene->default_surface->tag_update(scene);
  }
}

HdCyclesSession::~HdCyclesSession()
{
  if (_ownCyclesSession) {
    delete session;
  }
}

void HdCyclesSession::UpdateScene()
{
  Scene *const scene = session->scene.get();

  // Update background depending on presence of a background light
  if (scene->light_manager->need_update()) {
    Light *background_light = nullptr;
    bool have_lights = false;
    for (Object *object : scene->objects) {
      if (!object->get_geometry()->is_light()) {
        continue;
      }

      have_lights = true;

      Light *light = static_cast<Light *>(object->get_geometry());
      if (light->is_background_light()) {
        background_light = light;
        break;
      }
    }

    if (!background_light) {
      scene->background->set_shader(scene->default_background);
      scene->background->set_transparent(_transparentBackground.value_or(true));

      /* Set background color depending to non-zero value if there are no
       * lights in the scene, to match behavior of other renderers. */
      for (ShaderNode *node : scene->default_background->graph->nodes) {
        if (node->is_a(BackgroundNode::get_node_type())) {
          BackgroundNode *bgNode = static_cast<BackgroundNode *>(node);
          bgNode->set_color((have_lights) ? zero_float3() : make_float3(0.5f));
        }
      }
    }
    else {
      scene->background->set_shader(background_light->get_shader());
      scene->background->set_transparent(_transparentBackground.value_or(false));
    }

    scene->background->tag_update(scene);
  }
}

void HdCyclesSession::SetTransparentBackground(const std::optional<bool> transparent)
{
  _transparentBackground = transparent;

  // Re-evaluate the automatic transparency on the next scene update
  session->scene->light_manager->tag_update(session->scene.get(), LightManager::UPDATE_ALL);
}

void HdCyclesSession::SyncAovBindings(const HdRenderPassAovBindingVector &aovBindings)
{
  Scene *const scene = session->scene.get();

  // Delete all existing passes
  const vector<Pass *> &scene_passes = scene->passes;
  scene->delete_nodes(set<Pass *>(scene_passes.begin(), scene_passes.end()));

  // Update passes with requested AOV bindings
  _aovBindings = aovBindings;
  _passNames.clear();

  // Cryptomatte: the kernel writes every rank of the enabled types, in this order, so create all
  // those passes, also the ones no AOV reads.
  int cryptomatteTypes = CRYPT_NONE;
  int cryptomattePasses = 0;
  for (const HdRenderPassAovBinding &aovBinding : aovBindings) {
    CryptomatteType cryptomatteType;
    int index;
    if (ParseCryptomatte(GetSourceName(aovBinding).GetString(), cryptomatteType, index)) {
      cryptomatteTypes |= cryptomatteType;
      cryptomattePasses = std::max(cryptomattePasses, index + 1);
    }
  }
  scene->film->set_cryptomatte_passes(CryptomatteType(cryptomatteTypes));
  scene->film->set_cryptomatte_depth(cryptomattePasses);
  for (const auto &[prefix, cryptomatteType] : kCryptomatteTypes) {
    if (cryptomatteTypes & cryptomatteType) {
      for (int index = 0; index < cryptomattePasses; index++) {
        Pass *pass = scene->create_node<Pass>();
        pass->set_type(PASS_CRYPTOMATTE);
        pass->set_mode(PassMode::NOISY);
        pass->set_name(ustring(string_printf("%s%02d", prefix, index)));
      }
    }
  }

  std::unordered_map<int, const Pass *> passOfType;
  for (const HdRenderPassAovBinding &aovBinding : aovBindings) {
    const string sourceName = GetSourceName(aovBinding).GetString();
    CryptomatteType cryptomatteType;
    int index;
    if (ParseCryptomatte(sourceName, cryptomatteType, index)) {
      _passNames[aovBinding.aovName] = sourceName;
      continue;
    }

    const PassType type = AovToPassType(aovBinding);
    if (type == PASS_NONE) {
      TF_WARN("Unknown pass %s", aovBinding.aovName.GetText());
      continue;
    }

    const bool includeAlbedo = !GetLpe(aovBinding).empty();

    // The kernel writes one buffer per pass type, so AOVs of the same type (the beauty and a
    // "C.*[LO]" LPE, for example) read the first pass of that type. Shader AOVs have a pass each.
    if (type != PASS_AOV_COLOR && type != PASS_AOV_VALUE) {
      const auto existing = passOfType.find(type);
      if (existing != passOfType.end()) {
        if (existing->second->get_include_albedo() != includeAlbedo) {
          TF_WARN("AOV %s uses the same Cycles pass as %s, with%s albedo",
                  aovBinding.aovName.GetText(),
                  existing->second->get_name().c_str(),
                  existing->second->get_include_albedo() ? "" : "out");
        }
        _passNames[aovBinding.aovName] = existing->second->get_name().string();
        continue;
      }
    }

    const PassMode mode = PassMode::DENOISED;

    Pass *pass = scene->create_node<Pass>();
    pass->set_type(type);
    pass->set_mode(mode);
    pass->set_name(ustring(aovBinding.aovName.GetString()));
    pass->set_include_albedo(includeAlbedo);
    // Shader AOV passes are matched to AOV Output nodes by name
    if (type == PASS_AOV_COLOR || type == PASS_AOV_VALUE) {
      const TfToken sourceName = GetSourceName(aovBinding);
      if (sourceName != aovBinding.aovName) {
        pass->set_name(ustring(sourceName.GetString()));
        _passNames[aovBinding.aovName] = sourceName.GetString();
      }
    }
    passOfType[type] = pass;
  }
}

const string &HdCyclesSession::GetPassName(const TfToken &aovName) const
{
  const auto it = _passNames.find(aovName);
  return it != _passNames.end() ? it->second : aovName.GetString();
}

void HdCyclesSession::RemoveAovBinding(HdRenderBuffer *renderBuffer)
{
  for (HdRenderPassAovBinding &aovBinding : _aovBindings) {
    if (renderBuffer == aovBinding.renderBuffer) {
      aovBinding.renderBuffer = nullptr;
      break;
    }
  }

  if (renderBuffer == _displayAovBinding.renderBuffer) {
    _displayAovBinding.renderBuffer = nullptr;
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
