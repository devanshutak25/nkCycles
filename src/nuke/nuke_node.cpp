/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* CyclesRender: renders a scene of Nuke's USD-based 3D system with Cycles. */

#include "nuke/render.h"

#include <DDImage/CameraOp.h>
#include <DDImage/Format.h>
#include <DDImage/GeomOp.h>
#include <DDImage/Knobs.h>
#include <DDImage/LightOp.h>
#include <DDImage/MaterialOpI.h>
#include <DDImage/MetaData.h>
#include <DDImage/PlanarIop.h>
#include <usg/geom/Stage.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

using namespace DD::Image;

namespace {

const char *const kClass = "CyclesRender";
const char *const kHelp =
    "Renders the input 3D scene (USD-based 3D system) with Cycles.\n\n"
    "The camera knob takes the path of a camera prim, for example /Camera1. "
    "When empty, the first camera in the scene is used.";

const char *const kDevices[] = {"AUTO", "OPTIX", "CUDA", "CPU", nullptr};

/* Light passes of Blender, one layer per pass. Light passes are divided by the matching color
 * pass: beauty = (DiffDir + DiffInd) * DiffCol + ... + Emit + Env. */
struct LightPasses {
  const char *knob;
  const char *label;
  std::vector<std::pair<const char *, const char *>> layers; /* Layer, Cycles pass type. */
};
const LightPasses kLightPasses[] = {
    {"diffuse_passes",
     "diffuse",
     {{"DiffDir", "diffuse_direct"},
      {"DiffInd", "diffuse_indirect"},
      {"DiffCol", "diffuse_color"}}},
    {"glossy_passes",
     "glossy",
     {{"GlossDir", "glossy_direct"},
      {"GlossInd", "glossy_indirect"},
      {"GlossCol", "glossy_color"}}},
    {"transmission_passes",
     "transmission",
     {{"TransDir", "transmission_direct"},
      {"TransInd", "transmission_indirect"},
      {"TransCol", "transmission_color"}}},
    {"volume_passes",
     "volume",
     {{"VolumeDir", "volume_direct"}, {"VolumeInd", "volume_indirect"}}},
    {"emission_pass", "emission", {{"Emit", "emission"}}},
    {"environment_pass", "environment", {{"Env", "background"}}},
    {"ao_pass", "AO", {{"AO", "ao"}}},
};
constexpr int kNumLightPasses = sizeof(kLightPasses) / sizeof(kLightPasses[0]);

const char *const kCryptomatteLayers[] = {"CryptoObject", "CryptoMaterial", "CryptoAsset"};

/* Knobs of Nuke's lights and cameras that are not written into the stage (Nuke 15.0), with the
 * USD attribute they stand for. Light knobs mirror UsdLux inputs. */
struct KnobAttribute {
  const char *knob;
  const char *attribute;
  int size;
};
const KnobAttribute kLightKnobs[] = {
    {"color", "inputs:color", 3},
    {"intensity", "inputs:intensity", 1},
    {"exposure", "inputs:exposure", 1},
    {"enable_color_temperature", "inputs:enableColorTemperature", 1},
    {"color_temperature", "inputs:colorTemperature", 1},
    {"normalize", "inputs:normalize", 1},
    {"diffuse", "inputs:diffuse", 1},
    {"specular", "inputs:specular", 1},
    {"radius", "inputs:radius", 1},
    {"angle", "inputs:angle", 1},
    {"cone_angle", "inputs:shaping:cone:angle", 1},
    {"cone_softness", "inputs:shaping:cone:softness", 1},
};

/* Folder of this plugin, where the Cycles kernels are installed. */
std::string plugin_directory()
{
  std::string path;
#ifdef _WIN32
  HMODULE module = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(&plugin_directory),
                         &module))
  {
    char buffer[MAX_PATH];
    const DWORD size = GetModuleFileNameA(module, buffer, MAX_PATH);
    path.assign(buffer, size);
  }
#else
  Dl_info info;
  if (dladdr(reinterpret_cast<void *>(&plugin_directory), &info) && info.dli_fname) {
    path = info.dli_fname;
  }
#endif
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

/* getChannel() can add to Nuke's global channel list, which is not thread safe. */
Channel layer_channel(const std::string &layer, const char *name)
{
  static std::mutex mutex;
  const std::lock_guard<std::mutex> lock(mutex);
  return getChannel((layer + "." + name).c_str());
}

void add_knob_values(Op *op,
                     const KnobAttribute &knob_attribute,
                     const std::string &prim,
                     std::vector<nkcycles::AttributeValue> &values)
{
  Knob *knob = op->knob(knob_attribute.knob);
  if (!knob) {
    return;
  }
  nkcycles::AttributeValue value{prim, knob_attribute.attribute, {}};
  for (int i = 0; i < knob_attribute.size; i++) {
    value.value.push_back(knob->get_value_at(op->outputContext(), i));
  }
  values.push_back(value);
}

/* A Cycles pass copied into Nuke channels. */
struct PassOutput {
  std::string pass;
  int channels;
  int component;
  Channel channel;
};

/* A Nuke image used as texture. */
struct Texture {
  Iop *iop = nullptr;
  /* Format area, the texture. */
  Box box;
  /* Part of it with pixels, the bounding box. */
  Box fetch_box;
};

/* What a render needs from Nuke. Prepared by _validate on the main thread, since knobs,
 * validate() and request() must not be used from render threads. */
struct Prepared {
  Hash key;
  std::string error;
  std::string usd_text;
  nkcycles::RenderOptions options;
  std::vector<PassOutput> outputs;
  std::map<std::string, Texture> textures;
  MetaData::Bundle metadata;
};

/* RGBA pixels of the format area of an image, repeating the edge pixels outside its bounding
 * box, with alpha 1 when the image has none. The image is validated and requested by the node. */
std::shared_ptr<const nkcycles::Image> fetch_image(const Texture &texture)
{
  const Box &box = texture.box, &fetch = texture.fetch_box;
  auto image = std::make_shared<nkcycles::Image>();
  image->width = box.w();
  image->height = box.h();
  image->pixels.resize(size_t(box.w()) * box.h() * 4, 0.0f);
  if (fetch.w() <= 0 || fetch.h() <= 0) {
    return image;
  }
  ImagePlane plane(fetch, false, Mask_RGBA, 4);
  texture.iop->fetchPlane(plane);
  if (texture.iop->aborted()) {
    return nullptr;
  }
  const bool has_alpha = texture.iop->channels().contains(Chan_Alpha);
  float *pixel = image->pixels.data();
  for (int y = box.y(); y < box.t(); y++) {
    for (int x = box.x(); x < box.r(); x++) {
      for (int c = 0; c < 4; c++) {
        *pixel++ = (c == 3 && !has_alpha) ?
                       1.0f :
                       plane.at(fetch.clampx(x), fetch.clampy(y), Channel(Chan_Red + c));
      }
    }
  }
  return image;
}

class CyclesRender : public PlanarIop {
 public:
  static const Description description;

  explicit CyclesRender(Node *node) : PlanarIop(node)
  {
    _formats.format(nullptr);
  }

  const char *Class() const override
  {
    return kClass;
  }

  const char *node_help() const override
  {
    return kHelp;
  }

  int minimum_inputs() const override
  {
    return 1;
  }

  int maximum_inputs() const override
  {
    return 1;
  }

  bool test_input(int /*input*/, Op *op) const override
  {
    return op_cast<GeomOp *>(op) != nullptr;
  }

  Op *default_input(int /*input*/) const override
  {
    return nullptr;
  }

  const char *input_label(int /*input*/, char * /*buffer*/) const override
  {
    return "scn";
  }

  void knobs(Knob_Callback f) override
  {
    Format_knob(f, &_formats, "format");
    String_knob(f, &_camera, "camera", "camera");
    Tooltip(f, "Camera prim path, for example /Camera1. Empty uses the first camera.");
    Int_knob(f, &_samples, "samples");
    SetRange(f, 1, 4096);
    Enumeration_knob(f, &_device, kDevices, "device");
    Tooltip(f, "AUTO uses the best GPU (OptiX, then CUDA), or the CPU when there is none.");
    Bool_knob(f, &_denoise, "denoise");
    Bool_knob(f, &_depth_of_field, "depth_of_field", "depth of field");
    Tooltip(f, "Focus at the focal distance of the camera, blurred by its f-stop.");

    Divider(f, "outputs");
    Channel_knob(f, _depth_channel, 1, "depth_channel", "depth");
    Tooltip(f, "1/z in scene units, 0 for the background, like ScanlineRender.");
    Channel_knob(f, _normal_channel, 3, "N_channel", "normal");
    Tooltip(f, "World space shading normal.");
    Channel_knob(f, _position_channel, 3, "P_channel", "position");
    Tooltip(f, "World space position in scene units.");
    Channel_knob(f, _albedo_channel, 3, "albedo_channel", "albedo");
    Tooltip(f, "Surface color, as used by the denoiser.");
    for (int i = 0; i < kNumLightPasses; i++) {
      Bool_knob(f, &_light_passes[i], kLightPasses[i].knob, kLightPasses[i].label);
      if (i == 0) {
        SetFlags(f, Knob::STARTLINE);
        Tooltip(f,
                "Light passes in layers named like Blender's (DiffDir, DiffInd, DiffCol, ...).");
      }
    }
    Bool_knob(f, &_cryptomatte[0], "crypto_object", "cryptomatte object");
    SetFlags(f, Knob::STARTLINE);
    Tooltip(f, "Cryptomatte layers for Nuke's Cryptomatte node.");
    Bool_knob(f, &_cryptomatte[1], "crypto_material", "material");
    Bool_knob(f, &_cryptomatte[2], "crypto_asset", "asset");
    Int_knob(f, &_cryptomatte_levels, "crypto_levels", "levels");
    SetRange(f, 2, 16);
    Tooltip(f, "Number of overlapping IDs per pixel, two per layer.");
  }

  /* The scene can animate without animated knobs, like an imported USD file. */
  void append(Hash &hash) override
  {
    hash.append(outputContext().frame());
    enableVaryingOutputHash();
  }

  void _validate(bool for_real) override
  {
    if (_formats.format()) {
      info_.format(*_formats.format());
      info_.full_size_format(*_formats.fullSizeFormat());
    }
    info_.set(format());

    std::vector<nkcycles::PassRequest> passes;
    std::vector<PassOutput> outputs;
    for (int i = 0; i < 4; i++) {
      outputs.push_back({"combined", 4, i, Channel(Chan_Red + i)});
    }
    auto add_pass =
        [&](const std::string &name, const char *type, const Channel *channels, int count) {
          if (std::count(channels, channels + count, Chan_Black) == count) {
            return;
          }
          passes.push_back({name, type, count});
          for (int i = 0; i < count; i++) {
            if (channels[i] != Chan_Black) {
              outputs.push_back({name, count, i, channels[i]});
            }
          }
        };
    add_pass("depth", "depth", _depth_channel, 1);
    add_pass("normal", "normal", _normal_channel, 3);
    add_pass("position", "position", _position_channel, 3);
    add_pass("albedo", "denoising_albedo", _albedo_channel, 3);
    const char *const rgb[] = {"red", "green", "blue", "alpha"};
    for (int i = 0; i < kNumLightPasses; i++) {
      for (const auto &[layer, type] : kLightPasses[i].layers) {
        if (_light_passes[i]) {
          Channel channels[3];
          for (int c = 0; c < 3; c++) {
            channels[c] = layer_channel(layer, rgb[c]);
          }
          add_pass(layer, type, channels, 3);
        }
      }
    }
    /* Two IDs per 4 channel layer: id, coverage, id, coverage. */
    for (int i = 0; i < 3; i++) {
      for (int level = 0; _cryptomatte[i] && level < (_cryptomatte_levels + 1) / 2; level++) {
        char layer[32];
        snprintf(layer, sizeof(layer), "%s%02d", kCryptomatteLayers[i], level);
        Channel channels[4];
        for (int c = 0; c < 4; c++) {
          channels[c] = layer_channel(layer, rgb[c]);
        }
        add_pass(layer, "cryptomatte", channels, 4);
      }
    }

    ChannelSet channels;
    for (const PassOutput &output : outputs) {
      channels += output.channel;
    }
    info_.channels(channels);

    GeomOp *geo = op_cast<GeomOp *>(Op::input(0));
    if (geo) {
      geo->validate(for_real);
    }
    if (for_real) {
      prepare(geo, passes, outputs);
    }
  }

  void getRequests(const Box & /*box*/,
                   const ChannelSet & /*channels*/,
                   int count,
                   RequestOutput &request) const override
  {
    if (const std::shared_ptr<const Prepared> prepared = current()) {
      for (const auto &[path, texture] : prepared->textures) {
        if (texture.fetch_box.w() > 0 && texture.fetch_box.h() > 0) {
          request.request(texture.iop, texture.fetch_box, Mask_RGBA, count);
        }
      }
    }
  }

  bool useStripes() const override
  {
    return false;
  }

  bool renderFullPlanes() const override
  {
    return true;
  }

  /* Nuke asks for one plane per layer: all passes are rendered once and kept for the others. */
  void renderStripe(ImagePlane &plane) override
  {
    plane.makeWritable();
    foreach(z, plane.channels())
    {
      plane.fillChannel(z, 0.0f);
    }

    const Render render = render_once();
    if (!render.result) {
      if (!render.error.empty() && !aborted()) {
        error("Cycles: %s", render.error.c_str());
      }
      return;
    }

    /* Copy the bottom-up Cycles image into the plane, clipped to its bounds. */
    const int width = render.prepared->options.width, height = render.prepared->options.height;
    const Box &bounds = plane.bounds();
    const int x0 = std::max(bounds.x(), 0), x1 = std::min(bounds.r(), width);
    const int y0 = std::max(bounds.y(), 0), y1 = std::min(bounds.t(), height);
    const std::vector<PassOutput> &outputs = render.prepared->outputs;
    foreach(z, plane.channels())
    {
      const auto output = std::find_if(
          outputs.begin(), outputs.end(), [&](const PassOutput &o) { return o.channel == z; });
      if (output == outputs.end()) {
        continue;
      }
      const auto pixels = render.result->passes.find(output->pass);
      if (pixels == render.result->passes.end()) {
        continue;
      }
      const int chan = plane.chanNo(z);
      for (int y = y0; y < y1; y++) {
        const float *row = pixels->second.data() + size_t(y) * width * output->channels;
        for (int x = x0; x < x1; x++) {
          plane.writableAt(x, y, chan) = row[x * output->channels + output->component];
        }
      }
    }
  }

  /* Cryptomatte metadata, made from the scene without rendering it. */
  const MetaData::Bundle &_fetchMetaData(const char * /*key*/) override
  {
    validate(true);
    const std::lock_guard<std::mutex> lock(_prepared_mutex);
    /* Keep the returned bundle alive until the next call. */
    _metadata_owner = _prepared;
    return _prepared ? _prepared->metadata : _no_metadata;
  }

 private:
  /* A finished render and what it was made from. */
  struct Render {
    std::shared_ptr<const Prepared> prepared;
    std::shared_ptr<const nkcycles::RenderResult> result;
    std::string error;
  };

  std::shared_ptr<const Prepared> current() const
  {
    const std::lock_guard<std::mutex> lock(_prepared_mutex);
    return _prepared;
  }

  /* Values of the light and camera knobs Nuke does not write into the stage. */
  void collect_attributes(Op *op,
                          std::set<Op *> &visited,
                          std::vector<nkcycles::AttributeValue> &values) const
  {
    if (!op || !visited.insert(op).second) {
      return;
    }
    if (AxisOp *axis = dynamic_cast<AxisOp *>(op)) {
      const std::string prim = axis->primFullPath().asString();
      if (dynamic_cast<LightOp *>(op)) {
        for (const KnobAttribute &knob : kLightKnobs) {
          add_knob_values(op, knob, prim, values);
        }
        /* The image of an EnvironmentLight. */
        for (int i = 0; i < op->inputs(); i++) {
          Iop *image = op->uses_input(i) > 0.0f && op->input(i) ? op->input(i)->iop() : nullptr;
          if (image) {
            const std::string path = MaterialOpI::buildAssetPath(image);
            values.push_back({prim,
                              "inputs:texture:file",
                              {},
                              path.rfind("nkop:", 0) == 0 ? path : "nkop:" + path});
            break;
          }
        }
      }
      else if (dynamic_cast<CameraOp *>(op)) {
        Knob *near_knob = op->knob("near"), *far_knob = op->knob("far");
        if (near_knob && far_knob) {
          values.push_back({prim,
                            "clippingRange",
                            {near_knob->get_value_at(op->outputContext()),
                             far_knob->get_value_at(op->outputContext())}});
        }
        if (_depth_of_field) {
          add_knob_values(op, {"focal_point", "focusDistance", 1}, prim, values);
        }
      }
    }
    /* Inputs not in use may be out of date. */
    for (int i = 0; i < op->inputs(); i++) {
      if (op->uses_input(i) > 0.0f) {
        collect_attributes(op->input(i), visited, values);
      }
    }
  }

  /* Build the scene and gather what the render needs from Nuke, once per hash. */
  void prepare(GeomOp *geo,
               const std::vector<nkcycles::PassRequest> &passes,
               const std::vector<PassOutput> &outputs)
  {
    const Hash key = hash();
    if (const std::shared_ptr<const Prepared> prepared = current()) {
      if (prepared->key == key) {
        return;
      }
    }

    auto prepared = std::make_shared<Prepared>();
    prepared->key = key;
    prepared->outputs = outputs;
    usg::StageRef stage;
    if (geo) {
      GeomOp::buildStage(stage, usg::ArgSet(), geo);
    }
    if (!geo) {
      prepared->error = "No input scene";
    }
    else if (!stage || !stage->exportToString(prepared->usd_text, false)) {
      prepared->error = "Failed to export the input scene";
    }
    else {
      nkcycles::RenderOptions &options = prepared->options;
      options.width = format().width();
      options.height = format().height();
      options.samples = _samples;
      options.device = kDevices[_device];
      options.denoise = _denoise;
      options.camera = _camera;
      options.frame = outputContext().frame();
      options.passes = passes;
      std::set<Op *> visited;
      collect_attributes(geo, visited, options.attributes);

      /* Images of Nuke nodes used as textures, by their asset path in the scene. */
      std::set<std::string> paths;
      const std::string &text = prepared->usd_text;
      for (size_t start = text.find("@nkop:"); start != std::string::npos;
           start = text.find("@nkop:", start + 1))
      {
        const size_t end = text.find('@', start + 1);
        if (end == std::string::npos) {
          break;
        }
        paths.insert(text.substr(start + 1, end - start - 1));
      }
      for (const nkcycles::AttributeValue &value : options.attributes) {
        if (!value.asset.empty()) {
          paths.insert(value.asset);
        }
      }
      for (const std::string &path : paths) {
        Op *op = MaterialOpI::retrieveOpFromAssetPath(path);
        Iop *iop = op ? op->iop() : nullptr;
        if (!iop) {
          continue;
        }
        iop->validate(true);
        const Format &image_format = iop->format();
        Texture texture;
        texture.iop = iop;
        texture.box = Box(image_format.x(), image_format.y(), image_format.r(), image_format.t());
        texture.fetch_box = iop->info();
        texture.fetch_box.intersect(texture.box);
        if (texture.box.w() > 0 && texture.box.h() > 0) {
          prepared->textures[path] = texture;
        }
      }

      int cryptomatte_types = 0;
      for (int i = 0; i < 3; i++) {
        cryptomatte_types |= _cryptomatte[i] ? 1 << i : 0;
      }
      if (cryptomatte_types) {
        for (const auto &[name, value] :
             nkcycles::cryptomatte_metadata(prepared->usd_text, cryptomatte_types))
        {
          /* Keys as Nuke's Read gives them, Write writes them without "exr/". */
          prepared->metadata.setData("exr/" + name, value);
        }
      }
    }

    const std::lock_guard<std::mutex> lock(_prepared_mutex);
    _prepared = prepared;
  }

  /* Render all passes, once per hash. */
  Render render_once()
  {
    const std::shared_ptr<const Prepared> prepared = current();
    if (!prepared) {
      return {nullptr, nullptr, "Not validated"};
    }

    const std::lock_guard<std::mutex> lock(_render_mutex);
    if (_render.prepared && _render.prepared->key == prepared->key) {
      return _render;
    }
    if (!prepared->error.empty()) {
      _render = {prepared, nullptr, prepared->error};
      return _render;
    }

    static std::once_flag init_flag;
    std::call_once(init_flag, [] { nkcycles::init(plugin_directory()); });

    /* The path has the hash of the node, so images of the last render are reused. */
    nkcycles::RenderOptions options = prepared->options;
    std::map<std::string, std::shared_ptr<const nkcycles::Image>> images;
    for (const auto &[path, texture] : prepared->textures) {
      const auto image = _images.find(path);
      const std::shared_ptr<const nkcycles::Image> pixels = image != _images.end() ?
                                                                image->second :
                                                                fetch_image(texture);
      /* The image is also aborted when another tree it is in was. */
      if (!pixels || aborted()) {
        return {};
      }
      images[path] = pixels;
    }
    _images = images;
    options.images = images;
    options.aborted = [this] { return aborted(); };

    auto result = std::make_shared<nkcycles::RenderResult>();
    const std::string message = nkcycles::render(prepared->usd_text, options, *result);
    if (!message.empty() && aborted()) {
      /* Render again next time. */
      return {};
    }
    _render = {prepared, message.empty() ? result : nullptr, message};
    return _render;
  }

  FormatPair _formats;
  std::string _camera;
  int _samples = 64;
  int _device = 0;
  bool _denoise = true;
  bool _depth_of_field = false;
  Channel _depth_channel[1] = {Chan_Z};
  Channel _normal_channel[3] = {Chan_Black, Chan_Black, Chan_Black};
  Channel _position_channel[3] = {Chan_Black, Chan_Black, Chan_Black};
  Channel _albedo_channel[3] = {Chan_Black, Chan_Black, Chan_Black};
  bool _light_passes[kNumLightPasses] = {};
  bool _cryptomatte[3] = {};
  int _cryptomatte_levels = 6;

  mutable std::mutex _prepared_mutex;
  std::shared_ptr<const Prepared> _prepared;
  std::shared_ptr<const Prepared> _metadata_owner;
  const MetaData::Bundle _no_metadata;

  std::mutex _render_mutex;
  Render _render;
  std::map<std::string, std::shared_ptr<const nkcycles::Image>> _images;
};

Iop *build(Node *node)
{
  return new CyclesRender(node);
}

}  // namespace

const Iop::Description CyclesRender::description(kClass, "3D/CyclesRender", build);
