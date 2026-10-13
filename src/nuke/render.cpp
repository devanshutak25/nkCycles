/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "nuke/render.h"

#include "hydra/file_reader.h"
#include "hydra/render_delegate.h"

#include "scene/camera.h"
#include "scene/film.h"
#include "scene/image.h"
#include "scene/image_loader.h"
#include "scene/integrator.h"
#include "scene/object.h"
#include "scene/pass.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "session/buffers.h"
#include "session/output_driver.h"
#include "session/session.h"

#include "util/image_metadata.h"
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
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <OpenImageIO/filesystem.h>

#include <cstdlib>
#include <cstring>
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

/* Image of a Nuke node, fetched by the node. */
class NukeImageLoader : public ImageLoader {
 public:
  NukeImageLoader(const string &path, std::shared_ptr<const Image> image)
      : path_(path), image_(std::move(image))
  {
  }

  bool load_metadata(ImageMetaData &metadata,
                     const ImageLoaderParams & /*params*/,
                     Progress & /*progress*/) override
  {
    metadata.width = image_->width;
    metadata.height = image_->height;
    metadata.channels = 4;
    metadata.type = IMAGE_DATA_TYPE_FLOAT4;
    return true;
  }

  bool load_pixels(const ImageMetaData &metadata, void *pixels) override
  {
    memcpy(pixels, image_->pixels.data(), image_->pixels.size() * sizeof(float));
    metadata.conform_pixels(pixels);
    return true;
  }

  string name() const override
  {
    return path_;
  }

  bool equals(const ImageLoader &other) const override
  {
    const NukeImageLoader *loader = dynamic_cast<const NukeImageLoader *>(&other);
    return loader && loader->path_ == path_;
  }

 private:
  string path_;
  std::shared_ptr<const Image> image_;
};

/* Give the image texture nodes that read a Nuke image ("nkop:" asset path) its pixels. Nodes of
 * shaders that did not change keep the image they have. */
void set_nuke_images(Scene *scene,
                     const std::map<std::string, std::shared_ptr<const Image>> &images)
{
  if (images.empty()) {
    return;
  }
  /* Nuke's asset resolver may remove the URI scheme. */
  auto without_scheme = [](const string &path) {
    return string_startswith(path, "nkop:") ? path.substr(5) : path;
  };
  std::map<string, std::pair<string, std::shared_ptr<const Image>>> by_path;
  for (const auto &[path, image] : images) {
    by_path[without_scheme(path)] = {path, image};
  }
  for (Shader *shader : scene->shaders) {
    if (!shader->graph) {
      continue;
    }
    for (ShaderNode *node : shader->graph->nodes) {
      ImageHandle *handle = nullptr;
      ImageParams params;
      ustring filename;
      if (node->type == ImageTextureNode::get_node_type()) {
        ImageTextureNode *texture = static_cast<ImageTextureNode *>(node);
        handle = &texture->handle;
        params = texture->image_params();
        filename = texture->get_filename();
      }
      else if (node->type == EnvironmentTextureNode::get_node_type()) {
        EnvironmentTextureNode *texture = static_cast<EnvironmentTextureNode *>(node);
        handle = &texture->handle;
        params = texture->image_params();
        filename = texture->get_filename();
      }
      if (!handle || !handle->empty()) {
        continue;
      }
      const auto image = by_path.find(without_scheme(filename.string()));
      if (image != by_path.end() && image->second.second) {
        *handle = scene->image_manager->add_image(
            make_unique<NukeImageLoader>(image->second.first, image->second.second), params);
      }
    }
  }
}

/* Set a value on an attribute without authored value, converted to the attribute type. */
void set_attribute(const UsdStageRefPtr &stage, const AttributeValue &value)
{
  if ((value.value.empty() && value.asset.empty()) || !SdfPath::IsValidPathString(value.prim)) {
    return;
  }
  const UsdPrim prim = stage->GetPrimAtPath(SdfPath(value.prim));
  const UsdAttribute attr = prim ? prim.GetAttribute(TfToken(value.attribute)) : UsdAttribute();
  if (!attr || attr.HasAuthoredValue()) {
    return;
  }
  const std::vector<double> &v = value.value;
  const SdfValueTypeName type = attr.GetTypeName();
  if (type == SdfValueTypeNames->Asset) {
    attr.Set(SdfAssetPath(value.asset));
  }
  else if (v.empty()) {
    return;
  }
  else if (type == SdfValueTypeNames->Float) {
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

/* Session of the last render with its scene, so the next render only syncs the scene changes
 * and keeps the device, kernels and unchanged geometry. */
struct CachedSession {
  SessionParams params;
  bool denoise = false;
  unique_ptr<Session> session;
  SdfLayerRefPtr layer;
  UsdStageRefPtr stage;
  double meters_per_unit = 0.0;
  unique_ptr<HD_CYCLES_NS::HdCyclesFileReader> reader;
  std::vector<PassRequest> passes;
};

/* shortcut: one session for all nodes, alternating nodes sync a whole scene each time. Keep one
 * per node if that is common. Never deleted at exit, the CUDA library may already be unloaded. */
CachedSession *cached = nullptr;

/* One render at a time: renders share the session and the GPU, and Nuke may render several
 * frames at once. */
thread_mutex render_mutex;

/* Cryptomatte names of the last synced scene, which has the names Hydra gives to instanced
 * prims. Metadata is made before rendering, on Nuke's main thread. */
struct CryptomatteNames {
  thread_mutex mutex;
  std::set<string> objects, assets, materials;
} synced_names;

bool same_passes(const std::vector<PassRequest> &a, const std::vector<PassRequest> &b)
{
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto &x, const auto &y) {
    return x.name == y.name && x.type == y.type && x.channels == y.channels;
  });
}

/* Create the session passes. Cryptomatte passes are written for every rank of the enabled types,
 * in a fixed order, so they are created from the type mask and depth. Returns an error message. */
std::string create_passes(Scene *scene, const std::vector<PassRequest> &passes)
{
  scene->delete_nodes(set<Pass *>(scene->passes.begin(), scene->passes.end()));

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
  return "";
}

}  // namespace

void init(const std::string &root_path)
{
  path_init(root_path);
}

std::string render(const std::string &usd_text, const RenderOptions &options, RenderResult &result)
{
  result = RenderResult();

  /* The combined pass first, then the requested ones. */
  std::vector<PassRequest> passes = {{"combined", "combined", 4}};
  passes.insert(passes.end(), options.passes.begin(), options.passes.end());

  const thread_scoped_lock lock(render_mutex);

  HdRenderSettingsMap settings;
  settings[HD_CYCLES_NS::HdCyclesRenderSettingsTokens->device] = VtValue(options.device);
  SessionParams session_params = HD_CYCLES_NS::GetSessionParams(settings);
  session_params.background = true;
  session_params.samples = options.samples;
  /* Big images are rendered in tiles written to disk, then read back as one image. UTF-8 path. */
  session_params.temp_dir = OIIO::Filesystem::temp_directory_path();

  if (cached && cached->params.modified(session_params)) {
    delete cached;
    cached = nullptr;
  }
  if (!cached) {
    cached = new CachedSession();
    cached->params = session_params;
    cached->session = make_unique<Session>(session_params, SceneParams());
    cached->layer = SdfLayer::CreateAnonymous(".usda");
    cached->stage = UsdStage::Open(cached->layer);
  }
  Session *session = cached->session.get();
  Scene *scene = session->scene.get();
  const UsdStageRefPtr stage = cached->stage;

  /* The new scene replaces the layer content with fine grained edits, so the reader syncs only
   * the changed prims. */
  if (!stage || !cached->layer->ImportFromString(usd_text)) {
    delete cached;
    cached = nullptr;
    return "Failed to read the input scene";
  }
  /* Nuke works in meters and does not author the unit, USD would default to centimeters. */
  if (!stage->HasAuthoredMetadata(UsdGeomTokens->metersPerUnit)) {
    UsdGeomSetStageMetersPerUnit(stage, 1.0);
  }
  const double meters_per_unit = UsdGeomGetStageMetersPerUnit(stage);
  for (const AttributeValue &value : options.attributes) {
    set_attribute(stage, value);
  }
  /* Write the scene that is rendered, for bug reports. */
  if (const char *dump_path = getenv("NKCYCLES_DUMP_USD")) {
    cached->layer->Export(dump_path);
  }

  if (!cached->reader || cached->meters_per_unit != meters_per_unit) {
    cached->reader.reset();
    cached->reader = make_unique<HD_CYCLES_NS::HdCyclesFileReader>(
        session, stage, meters_per_unit);
    cached->meters_per_unit = meters_per_unit;
  }

  /* Image size first, the camera window is conformed to it when the scene is synced. */
  scene->camera->set_full_width(options.width);
  scene->camera->set_full_height(options.height);

  /* Nuke fits the horizontal aperture to the image width. */
  if (!cached->reader->sync(UsdTimeCode(options.frame),
                            options.camera.empty() ? SdfPath() : SdfPath(options.camera),
                            true,
                            CameraUtilMatchHorizontally))
  {
    return options.camera.empty() ? "No camera in the scene" : "No camera at " + options.camera;
  }
  scene->integrator->set_use_denoise(options.denoise);
  /* The session only raises the samples, but their count sets up the sampling pattern. */
  scene->integrator->set_aa_samples(options.samples);
  set_nuke_images(scene, options.images);
  {
    const thread_scoped_lock names_lock(synced_names.mutex);
    synced_names.objects.clear();
    synced_names.assets.clear();
    synced_names.materials.clear();
    for (const Object *object : scene->objects) {
      synced_names.objects.insert(object->name.string());
      synced_names.assets.insert(object->get_asset_name().string());
    }
    for (const Shader *shader : scene->shaders) {
      synced_names.materials.insert(shader->name.string());
    }
  }

  /* Without denoising the film makes the passes noisy, so denoising again needs new passes. */
  if (!same_passes(passes, cached->passes) || cached->denoise != options.denoise) {
    const std::string message = create_passes(scene, passes);
    cached->passes = message.empty() ? passes : std::vector<PassRequest>();
    cached->denoise = options.denoise;
    if (!message.empty()) {
      return message;
    }
  }

  session->progress.reset();
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
    OIIO::Filesystem::remove(filename);
  }

  session->set_output_driver(nullptr);
  session->progress.set_update_callback(nullptr);
  session->full_buffer_written_cb = nullptr;

  if (session->progress.get_cancel()) {
    result = RenderResult();
    return session->progress.get_cancel_message();
  }
  if (session->progress.get_error() || session->device->have_error()) {
    /* Start from a new session next time, device errors are not cleared. */
    const std::string message = session->progress.get_error() ?
                                    session->progress.get_error_message() :
                                    session->device->error_message();
    delete cached;
    cached = nullptr;
    result = RenderResult();
    return message;
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
  return "";
}

std::map<std::string, std::string> cryptomatte_metadata(const std::string &usd_text,
                                                        const int types)
{
  std::map<std::string, std::string> metadata;
  const SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
  const UsdStageRefPtr stage = layer->ImportFromString(usd_text) ? UsdStage::Open(layer) :
                                                                   UsdStageRefPtr();
  if (!stage) {
    return metadata;
  }
  /* The render names objects after their prim, assets after its parent and materials after
   * their prim. Instanced prims get other names, known once the scene was rendered. */
  std::set<string> objects, assets, materials;
  {
    const thread_scoped_lock names_lock(synced_names.mutex);
    objects = synced_names.objects;
    assets = synced_names.assets;
    materials = synced_names.materials;
  }
  materials.insert({"default_surface", "default_volume"});
  for (const UsdPrim &prim : stage->Traverse()) {
    if (prim.IsA<UsdGeomGprim>()) {
      objects.insert(prim.GetPath().GetString());
      assets.insert(prim.GetPath().GetParentPath().GetString());
    }
    else if (prim.GetTypeName() == "Material") {
      materials.insert(prim.GetPath().GetString());
    }
  }
  for (const auto &[layer_name, type] : kCryptomatteTypes) {
    if (!(types & type)) {
      continue;
    }
    const uint32_t hash = util_murmur_hash3(layer_name, strlen(layer_name), 0);
    const string prefix = string_printf("cryptomatte/%08x", hash).substr(0, 19) + "/";
    metadata[prefix + "name"] = layer_name;
    metadata[prefix + "hash"] = "MurmurHash3_32";
    metadata[prefix + "conversion"] = "uint32_to_float32";
    metadata[prefix + "manifest"] = cryptomatte_manifest(type == CRYPT_OBJECT ? objects :
                                                         type == CRYPT_ASSET  ? assets :
                                                                                materials);
  }
  return metadata;
}

}  // namespace nkcycles
