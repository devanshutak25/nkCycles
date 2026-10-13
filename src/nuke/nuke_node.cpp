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

/* RGBA pixels of the format area of an image, with alpha 1 when the image has none. */
std::shared_ptr<const nkcycles::Image> fetch_image(Iop *iop)
{
  iop->validate(true);
  const Format &format = iop->format();
  const Box box(format.x(), format.y(), format.r(), format.t());
  if (box.w() <= 0 || box.h() <= 0) {
    return nullptr;
  }
  iop->request(box, Mask_RGBA, 1);
  ImagePlane plane(box, false, Mask_RGBA, 4);
  iop->fetchPlane(plane);
  if (iop->aborted()) {
    return nullptr;
  }
  const bool has_alpha = iop->channels().contains(Chan_Alpha);
  auto image = std::make_shared<nkcycles::Image>();
  image->width = box.w();
  image->height = box.h();
  image->pixels.resize(size_t(box.w()) * box.h() * 4);
  float *pixel = image->pixels.data();
  for (int y = box.y(); y < box.t(); y++) {
    for (int x = box.x(); x < box.r(); x++) {
      for (int c = 0; c < 4; c++) {
        *pixel++ = (c == 3 && !has_alpha) ? 1.0f : plane.at(x, y, Channel(Chan_Red + c));
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

  void _validate(bool for_real) override
  {
    if (_formats.format()) {
      info_.format(*_formats.format());
      info_.full_size_format(*_formats.fullSizeFormat());
    }
    info_.set(format());

    _passes.clear();
    _outputs.clear();
    for (int i = 0; i < 4; i++) {
      _outputs.push_back({"combined", 4, i, Channel(Chan_Red + i)});
    }
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
    for (const Output &output : _outputs) {
      channels += output.channel;
    }
    info_.channels(channels);

    if (GeomOp *geo = op_cast<GeomOp *>(Op::input(0))) {
      geo->validate(for_real);
    }
  }

  void getRequests(const Box & /*box*/,
                   const ChannelSet & /*channels*/,
                   int /*count*/,
                   RequestOutput & /*request*/) const override
  {
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

    if (!render_once()) {
      if (!_error.empty() && !aborted()) {
        error("Cycles: %s", _error.c_str());
      }
      return;
    }

    /* Copy the bottom-up Cycles image into the plane, clipped to its bounds. */
    const int width = format().width(), height = format().height();
    const Box &bounds = plane.bounds();
    const int x0 = std::max(bounds.x(), 0), x1 = std::min(bounds.r(), width);
    const int y0 = std::max(bounds.y(), 0), y1 = std::min(bounds.t(), height);
    foreach(z, plane.channels())
    {
      const auto output = std::find_if(
          _outputs.begin(), _outputs.end(), [&](const Output &o) { return o.channel == z; });
      if (output == _outputs.end()) {
        continue;
      }
      const auto pixels = _result.passes.find(output->pass);
      if (pixels == _result.passes.end()) {
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

  /* Cryptomatte metadata, rendering the image when it was not yet. */
  const MetaData::Bundle &_fetchMetaData(const char * /*key*/) override
  {
    _metadata = MetaData::Bundle();
    if (std::find(_cryptomatte, _cryptomatte + 3, true) == _cryptomatte + 3) {
      return _metadata;
    }
    validate(true);
    if (render_once()) {
      for (const auto &[key, value] : _result.metadata) {
        /* Keys as Nuke's Read gives them, Write writes them without "exr/". */
        _metadata.setData("exr/" + key, value);
      }
    }
    return _metadata;
  }

 private:
  /* A Cycles pass copied into Nuke channels. */
  struct Output {
    std::string pass;
    int channels;
    int component;
    Channel channel;
  };

  void add_pass(const std::string &name, const char *type, const Channel *channels, int count)
  {
    if (std::count(channels, channels + count, Chan_Black) == count) {
      return;
    }
    _passes.push_back({name, type, count});
    for (int i = 0; i < count; i++) {
      if (channels[i] != Chan_Black) {
        _outputs.push_back({name, count, i, channels[i]});
      }
    }
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
          if (Iop *image = op->input(i) ? op->input(i)->iop() : nullptr) {
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
    for (int i = 0; i < op->inputs(); i++) {
      collect_attributes(op->input(i), visited, values);
    }
  }

  /* Images of Nuke nodes the scene uses as textures, by asset path. The path has the hash of the
   * node, so images of the last render are reused. */
  std::map<std::string, std::shared_ptr<const nkcycles::Image>> fetch_images(
      const std::string &usd_text, const std::vector<nkcycles::AttributeValue> &attributes)
  {
    std::set<std::string> paths;
    for (size_t start = usd_text.find("@nkop:"); start != std::string::npos;
         start = usd_text.find("@nkop:", start + 1))
    {
      const size_t end = usd_text.find('@', start + 1);
      if (end == std::string::npos) {
        break;
      }
      paths.insert(usd_text.substr(start + 1, end - start - 1));
    }
    for (const nkcycles::AttributeValue &value : attributes) {
      if (!value.asset.empty()) {
        paths.insert(value.asset);
      }
    }

    std::map<std::string, std::shared_ptr<const nkcycles::Image>> images;
    for (const std::string &path : paths) {
      const auto image = _images.find(path);
      if (image != _images.end()) {
        images[path] = image->second;
        continue;
      }
      Op *op = MaterialOpI::retrieveOpFromAssetPath(path);
      if (Iop *iop = op ? op->iop() : nullptr) {
        if (std::shared_ptr<const nkcycles::Image> pixels = fetch_image(iop)) {
          images[path] = pixels;
        }
      }
    }
    _images = images;
    return images;
  }

  /* Render all passes, once per hash. Returns false when there is no image. */
  bool render_once()
  {
    const std::lock_guard<std::mutex> lock(_render_mutex);
    Hash key = hash();
    key.append(outputContext().frame());
    if (_rendered && _rendered_key == key) {
      return _error.empty();
    }
    _rendered = false;
    _result = nkcycles::RenderResult();
    _error.clear();

    GeomOp *geo = op_cast<GeomOp *>(Op::input(0));
    if (!geo) {
      _error = "No input scene";
      return false;
    }

    static std::once_flag init_flag;
    std::call_once(init_flag, [] { nkcycles::init(plugin_directory()); });

    usg::StageRef stage;
    GeomOp::buildStage(stage, usg::ArgSet(), geo);
    std::string usd_text;
    if (!stage || !stage->exportToString(usd_text, false)) {
      _error = "Failed to export the input scene";
      _rendered = true;
      _rendered_key = key;
      return false;
    }

    const Format &fmt = format();
    nkcycles::RenderOptions options;
    options.width = fmt.width();
    options.height = fmt.height();
    options.samples = _samples;
    options.device = kDevices[_device];
    options.denoise = _denoise;
    options.camera = _camera;
    options.frame = outputContext().frame();
    options.passes = _passes;
    std::set<Op *> visited;
    collect_attributes(geo, visited, options.attributes);
    options.images = fetch_images(usd_text, options.attributes);
    if (aborted()) {
      return false;
    }
    options.aborted = [this] { return aborted(); };

    _error = nkcycles::render(usd_text, options, _result);
    if (!_error.empty() && aborted()) {
      /* Render again next time. */
      return false;
    }
    _rendered = true;
    _rendered_key = key;
    return _error.empty();
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

  /* Set by _validate. */
  std::vector<nkcycles::PassRequest> _passes;
  std::vector<Output> _outputs;

  std::mutex _render_mutex;
  bool _rendered = false;
  Hash _rendered_key;
  nkcycles::RenderResult _result;
  std::string _error;
  MetaData::Bundle _metadata;
  std::map<std::string, std::shared_ptr<const nkcycles::Image>> _images;
};

Iop *build(Node *node)
{
  return new CyclesRender(node);
}

}  // namespace

const Iop::Description CyclesRender::description(kClass, "3D/CyclesRender", build);
