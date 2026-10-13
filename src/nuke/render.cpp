/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "nuke/render.h"

#include "hydra/file_reader.h"
#include "hydra/render_delegate.h"

#include "scene/camera.h"
#include "scene/film.h"
#include "scene/integrator.h"
#include "scene/object.h"
#include "scene/pass.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "session/buffers.h"
#include "session/output_driver.h"
#include "session/session.h"

#include "util/murmurhash.h"
#include "util/path.h"
#include "util/string.h"
#include "util/thread.h"
#include "util/unique_ptr.h"

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

namespace nkcycles {

using namespace ccl;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {

/* Copies the requested passes of the finished image. */
class BufferOutputDriver : public OutputDriver {
 public:
  BufferOutputDriver(const std::vector<PassRequest> &passes, RenderResult &result)
      : passes_(passes), result_(result)
  {
  }

  void write_render_tile(const Tile &tile) override
  {
    /* Only the full image, no intermediate tiles. Tiled renders give the full image when
     * reading back the tiles from disk. */
    if (!(tile.size == tile.full_size)) {
      return;
    }
    const size_t num_pixels = size_t(tile.size.x) * tile.size.y;
    for (const PassRequest &pass : passes_) {
      std::vector<float> &pixels = result_.passes[pass.name];
      pixels.resize(num_pixels * pass.channels);
      if (!tile.get_pass_pixels(pass.name, pass.channels, pixels.data())) {
        result_.passes.erase(pass.name);
      }
    }
  }

 private:
  const std::vector<PassRequest> &passes_;
  RenderResult &result_;
};

/* Cryptomatte layers of Blender, the kernel writes the enabled types in this order. */
const std::pair<const char *, CryptomatteType> kCryptomatteTypes[] = {
    {"CryptoObject", CRYPT_OBJECT},
    {"CryptoMaterial", CRYPT_MATERIAL},
    {"CryptoAsset", CRYPT_ASSET},
};

/* Manifest of the cryptomatte specification. Unlike Cycles' own, the IDs are the bits of the
 * float the kernel writes, which differ from the hash for 2 hashes in 256: readers like Nuke's
 * Cryptomatte drop entries whose ID does not match. */
string cryptomatte_manifest(const std::set<string> &names)
{
  string manifest = "{";
  for (const string &name : names) {
    if (name.empty()) {
      continue;
    }
    const float id = util_hash_to_float(util_murmur_hash3(name.c_str(), name.length(), 0));
    uint32_t bits;
    memcpy(&bits, &id, sizeof(bits));
    string escaped;
    for (const char c : name) {
      if (c == '"' || c == '\\') {
        escaped += '\\';
      }
      escaped += c;
    }
    manifest += string_printf(
        "%s\"%s\":\"%08x\"", manifest.size() > 1 ? "," : "", escaped.c_str(), bits);
  }
  return manifest + "}";
}

void add_cryptomatte_metadata(Scene *scene, const int types, RenderResult &result)
{
  for (const auto &[layer, type] : kCryptomatteTypes) {
    if (!(types & type)) {
      continue;
    }
    std::set<string> names;
    if (type == CRYPT_MATERIAL) {
      for (const Shader *shader : scene->shaders) {
        names.insert(shader->name.string());
      }
    }
    else {
      for (const Object *object : scene->objects) {
        names.insert((type == CRYPT_OBJECT ? object->name : object->get_asset_name()).string());
      }
    }
    const uint32_t hash = util_murmur_hash3(layer, strlen(layer), 0);
    const string prefix = string_printf("cryptomatte/%08x", hash).substr(0, 19) + "/";
    result.metadata[prefix + "name"] = layer;
    result.metadata[prefix + "hash"] = "MurmurHash3_32";
    result.metadata[prefix + "conversion"] = "uint32_to_float32";
    result.metadata[prefix + "manifest"] = cryptomatte_manifest(names);
  }
}

/* Set a value on an attribute without authored value, converted to the attribute type. */
void set_attribute(const UsdStageRefPtr &stage, const AttributeValue &value)
{
  if (value.value.empty() || !SdfPath::IsValidPathString(value.prim)) {
    return;
  }
  const UsdPrim prim = stage->GetPrimAtPath(SdfPath(value.prim));
  const UsdAttribute attr = prim ? prim.GetAttribute(TfToken(value.attribute)) : UsdAttribute();
  if (!attr || attr.HasAuthoredValue()) {
    return;
  }
  const std::vector<double> &v = value.value;
  const SdfValueTypeName type = attr.GetTypeName();
  if (type == SdfValueTypeNames->Float) {
    attr.Set(float(v[0]));
  }
  else if (type == SdfValueTypeNames->Double) {
    attr.Set(v[0]);
  }
  else if (type == SdfValueTypeNames->Bool) {
    attr.Set(v[0] != 0.0);
  }
  else if (type == SdfValueTypeNames->Float2 && v.size() >= 2) {
    attr.Set(GfVec2f(v[0], v[1]));
  }
  else if (type.GetType() == SdfValueTypeNames->Float3.GetType() && v.size() >= 3) {
    /* Color3f, Vector3f, ... */
    attr.Set(GfVec3f(v[0], v[1], v[2]));
  }
}

/* One render at a time: renders share the GPU, and Nuke may render several frames at once. */
thread_mutex render_mutex;

}  // namespace

void init(const std::string &root_path)
{
  path_init(root_path);
}

std::string render(const std::string &usd_text, const RenderOptions &options, RenderResult &result)
{
  result = RenderResult();

  /* Write the scene as received from Nuke, for bug reports. */
  if (const char *dump_path = getenv("NKCYCLES_DUMP_USD")) {
    std::ofstream(dump_path) << usd_text;
  }

  const SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
  if (!layer->ImportFromString(usd_text)) {
    return "Failed to read the input scene";
  }
  const UsdStageRefPtr stage = UsdStage::Open(layer);
  if (!stage) {
    return "Failed to open the input scene";
  }
  /* Nuke works in meters and does not author the unit, USD would default to centimeters. */
  if (!stage->HasAuthoredMetadata(UsdGeomTokens->metersPerUnit)) {
    UsdGeomSetStageMetersPerUnit(stage, 1.0);
  }
  const double meters_per_unit = UsdGeomGetStageMetersPerUnit(stage);
  for (const AttributeValue &value : options.attributes) {
    set_attribute(stage, value);
  }

  /* The combined pass first, then the requested ones. */
  std::vector<PassRequest> passes = {{"combined", "combined", 4}};
  passes.insert(passes.end(), options.passes.begin(), options.passes.end());

  const thread_scoped_lock lock(render_mutex);

  HdRenderSettingsMap settings;
  settings[HD_CYCLES_NS::HdCyclesRenderSettingsTokens->device] = VtValue(options.device);
  SessionParams session_params = HD_CYCLES_NS::GetSessionParams(settings);
  session_params.background = true;
  session_params.samples = options.samples;
  /* Big images are rendered in tiles written to disk, then read back as one image. */
  session_params.temp_dir = std::filesystem::temp_directory_path().string();

  /* shortcut: a new session (device, kernels, scene) per render, keep one per node for faster
   * re-renders. */
  const unique_ptr<Session> session = make_unique<Session>(session_params, SceneParams());
  session->set_output_driver(make_unique<BufferOutputDriver>(passes, result));
  session->progress.set_update_callback([&] {
    if (options.aborted && options.aborted()) {
      session->progress.set_cancel("Cancelled");
    }
  });
  std::vector<string> tile_files;
  session->full_buffer_written_cb = [&](string_view filename) {
    tile_files.emplace_back(filename);
  };

  /* Image size first, the camera window is conformed to it when the scene is read. */
  Scene *scene = session->scene.get();
  scene->camera->set_full_width(options.width);
  scene->camera->set_full_height(options.height);

  /* Nuke fits the horizontal aperture to the image width. */
  HD_CYCLES_NS::HdCyclesFileReader::read(session.get(),
                                         stage,
                                         UsdTimeCode(options.frame),
                                         options.camera.empty() ? SdfPath() :
                                                                  SdfPath(options.camera),
                                         true,
                                         CameraUtilMatchHorizontally);
  scene->integrator->set_use_denoise(options.denoise);

  /* Passes. Cryptomatte ones are written for every rank of the enabled types, in a fixed
   * order, so they are created from the type mask and depth. */
  const NodeEnum *pass_types = Pass::get_type_enum();
  int cryptomatte_types = CRYPT_NONE;
  int cryptomatte_depth = 0;
  for (const PassRequest &request : passes) {
    if (request.type == "cryptomatte") {
      for (const auto &[layer, type] : kCryptomatteTypes) {
        const size_t length = strlen(layer);
        if (request.name.size() == length + 2 && request.name.compare(0, length, layer) == 0) {
          cryptomatte_types |= type;
          cryptomatte_depth = std::max(cryptomatte_depth, atoi(request.name.c_str() + length) + 1);
        }
      }
      continue;
    }
    if (!pass_types->exists(ustring(request.type))) {
      return "Unknown pass type " + request.type;
    }
    Pass *pass = scene->create_node<Pass>();
    pass->set_type(PassType((*pass_types)[ustring(request.type)]));
    /* Only the beauty is denoised, the light passes would no longer add up to it. */
    pass->set_mode(request.type == "combined" ? PassMode::DENOISED : PassMode::NOISY);
    pass->set_name(ustring(request.name));
  }
  scene->film->set_cryptomatte_passes(CryptomatteType(cryptomatte_types));
  scene->film->set_cryptomatte_depth(cryptomatte_depth);
  for (const auto &[layer, type] : kCryptomatteTypes) {
    for (int index = 0; (cryptomatte_types & type) && index < cryptomatte_depth; index++) {
      Pass *pass = scene->create_node<Pass>();
      pass->set_type(PASS_CRYPTOMATTE);
      pass->set_mode(PassMode::NOISY);
      pass->set_name(ustring(string_printf("%s%02d", layer, index)));
    }
  }

  BufferParams buffer_params;
  buffer_params.width = buffer_params.full_width = options.width;
  buffer_params.height = buffer_params.full_height = options.height;

  session->reset(session_params, buffer_params);
  session->start();
  session->wait();

  for (const string &filename : tile_files) {
    if (!session->progress.get_cancel()) {
      session->process_full_buffer_from_disk(filename);
    }
    path_remove(filename);
  }

  if (session->progress.get_cancel()) {
    result = RenderResult();
    return session->progress.get_cancel_message();
  }
  if (session->progress.get_error()) {
    result = RenderResult();
    return session->progress.get_error_message();
  }
  if (result.passes["combined"].size() != size_t(options.width) * options.height * 4) {
    result = RenderResult();
    return "Cycles produced no image";
  }

  /* Nuke conventions: depth is 1/z in stage units, 0 for the background (Cycles gives 1e10),
   * position is in stage units. */
  for (const PassRequest &request : options.passes) {
    auto pixels = result.passes.find(request.name);
    if (pixels == result.passes.end()) {
      continue;
    }
    for (float &v : pixels->second) {
      if (request.type == "depth") {
        v = (v > 0.0f && v < 1e9f) ? float(meters_per_unit / v) : 0.0f;
      }
      else if (request.type == "position") {
        v = float(v / meters_per_unit);
      }
    }
  }
  add_cryptomatte_metadata(scene, cryptomatte_types, result);
  return "";
}

}  // namespace nkcycles
