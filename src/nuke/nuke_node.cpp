/* SPDX-FileCopyrightText: 2026 Devanshu Tak
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
#include <DDImage/Shutter.h>
#include <DDImage/ddImageVersionNumbers.h>
#if kDDImageVersionInteger >= 161000
#  include <DDImage/CameraSceneOp.h>
#  include <DDImage/GeometryProviderI.h>
#  include <DDImage/LightSceneOp.h>
#elif kDDImageVersionInteger >= 160000
#  error "Nuke 16.0 is not supported yet: the stage and prim path functions differ."
#endif
#include <usg/geom/Stage.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>

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
    "The camera knob takes the path of a camera prim, as shown in the Scene Graph. "
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

/* Nuke writes some arrays with a scalar type ("uniform int inputs:channels = [1, 2, 3, 4]" of
 * FillShader, WireframeShader and MergeLayerShader), which USD cannot read back: give them the
 * array type. */
void fix_array_types(std::string &text)
{
  static const std::set<std::string_view> scalar_types = {"bool",
                                                          "int",
                                                          "uint",
                                                          "int64",
                                                          "uint64",
                                                          "half",
                                                          "float",
                                                          "double",
                                                          "token",
                                                          "string",
                                                          "asset"};
  const std::string_view view(text);
  std::string fixed;
  size_t copied = 0;
  for (size_t line = 0; line < view.size();) {
    const size_t end = std::min(view.find('\n', line), view.size());
    size_t type = view.find_first_not_of(" \t", line);
    for (const std::string_view qualifier : {"custom ", "uniform "}) {
      if (type < end && view.substr(type, qualifier.size()) == qualifier) {
        type = view.find_first_not_of(' ', type + qualifier.size());
      }
    }
    const size_t type_end = std::min(view.find(' ', type), end);
    if (type < type_end && scalar_types.count(view.substr(type, type_end - type))) {
      const size_t name_end = view.find_first_of(" =", type_end + 1);
      const std::string_view name = view.substr(type_end + 1, name_end - type_end - 1);
      const std::string_view connect = ".connect";
      const bool is_connection = name.size() >= connect.size() &&
                                 name.substr(name.size() - connect.size()) == connect;
      if (name_end < end && view.substr(name_end, 4) == " = [" && !is_connection) {
        fixed.append(view.substr(copied, type_end - copied)).append("[]");
        copied = type_end;
      }
    }
    line = end + 1;
  }
  if (copied > 0) {
    text = fixed.append(view.substr(copied));
  }
}

/* Prim paths separated by white space. */
std::vector<std::string> split_paths(const std::string &text)
{
  std::vector<std::string> paths;
  std::istringstream stream(text);
  for (std::string path; stream >> path;) {
    paths.push_back(path);
  }
  return paths;
}

/* Path of the USD light or camera prim of a Nuke light or camera, empty for other ops. */
std::string light_or_camera_prim(Op *op)
{
#if kDDImageVersionInteger >= 161000
  if (LightSceneOp *light = dynamic_cast<LightSceneOp *>(op)) {
    return light->buildLightSchemaPrimPath().asString();
  }
  if (CameraSceneOp *camera = dynamic_cast<CameraSceneOp *>(op)) {
    return camera->buildCameraSchemaPrimPath().asString();
  }
#else
  if (AxisOp *axis = dynamic_cast<AxisOp *>(op)) {
    return axis->primFullPath().asString();
  }
#endif
  return std::string();
}

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
    Tooltip(f, "Camera prim path, as shown in the Scene Graph. Empty uses the first camera.");
    String_knob(f, &_holdout, "holdout", "holdout");
    Tooltip(f,
            "Prim paths, separated by spaces, of objects cut out of the render: they hide what "
            "is behind them, with alpha 0, but still cast shadows and show in reflections.");
    Int_knob(f, &_samples, "samples");
    SetRange(f, 1, 4096);
    Enumeration_knob(f, &_device, kDevices, "device");
    Tooltip(f, "AUTO uses the best GPU (OptiX, then CUDA), or the CPU when there is none.");
    Bool_knob(f, &_denoise, "denoise");
    Bool_knob(f, &_depth_of_field, "depth_of_field", "depth of field");
    Tooltip(f, "Focus at the focal distance of the camera, blurred by its f-stop.");
    Int_knob(f, &_shutter_steps, "shutter_steps", "motion blur steps");
    SetFlags(f, Knob::EARLY_STORE);
    SetRange(f, 0, 16);
    Tooltip(f,
            "Motion blur over the shutter: 0 off, 1 a straight blur, more steps follow curved "
            "motion. The scene is read at the center of the shutter and evenly around it.");
    _shutter.knobs(f, true);

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

  /* Motion blur: the number of intervals between the frames the scene is read at, over the
   * shutter. Even, so the center is one of the frames, and 0 without motion blur. */
  int shutter_intervals() const
  {
    if (_shutter_steps <= 0 || _shutter.getDuration() <= 0.0) {
      return 0;
    }
#if kDDImageVersionInteger >= 161000
    /* At most the 129 motion steps of Cycles. */
    return std::min(_shutter_steps + _shutter_steps % 2, 128);
#else
    /* Nuke 15 writes no frames between the shutter open, center and close. */
    return 2;
#endif
  }

  /* The scene input once per frame over the shutter. */
  int split_input(int input) const override
  {
    return input == 0 ? shutter_intervals() + 1 : 1;
  }

  const OutputContext &inputContext(int input, int offset, OutputContext &context) const override
  {
    context = outputContext();
    const int intervals = shutter_intervals();
    if (input == 0 && intervals > 0) {
      context.setFrame(context.frame() + _shutter.calcOffset() +
                       _shutter.getDuration() * offset / intervals);
    }
    return context;
  }

  const OutputContext *inputUIContext(int /*input*/, OutputContext & /*context*/) const override
  {
    return &outputContext();
  }

  /* The scene input at the frame the render is centered on. */
  GeomOp *scene_input() const
  {
    return op_cast<GeomOp *>(Op::input(0, split_input(0) / 2));
  }

  /* The scene can animate without animated knobs, like an imported USD file. */
  void append(Hash &hash) override
  {
    hash.append(outputContext().frame());
    /* An upstream FrameHold or TimeOffset changes the frames the scene is read at. */
    for (int offset = 0; offset < split_input(0); offset++) {
      if (Op *scene = Op::input(0, offset)) {
        hash.append(scene->outputContext().frame());
      }
    }
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

    std::vector<GeomOp *> scenes;
    for (int offset = 0; offset < split_input(0); offset++) {
      if (GeomOp *scene = op_cast<GeomOp *>(Op::input(0, offset))) {
        scene->validate(for_real);
        scenes.push_back(scene);
      }
    }
    if (for_real) {
      prepare(scene_input(), scenes, passes, outputs);
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
  void collect_attributes(Op *op, std::set<Op *> &visited, nkcycles::RenderOptions &options) const
  {
    if (!op || !visited.insert(op).second) {
      return;
    }
    std::vector<nkcycles::AttributeValue> &values = options.attributes;
    const std::string prim = light_or_camera_prim(op);
    if (!prim.empty()) {
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
        /* Nuke's lens is in millimeters, USD's in tenths of a scene unit: scale the f-stop so
         * the aperture is the one of the millimeter lens (Nuke 16.1+ authors the f-stop). */
        Knob *fstop_knob = op->knob("fstop");
        if (_depth_of_field && fstop_knob) {
          double lens_scale = 100.0;
#if kDDImageVersionInteger >= 161000
          /* Scene units per meter, 100 for centimeters. */
          if (Knob *world_to_meters = op->knob("world_to_meters")) {
            const double units_per_meter = world_to_meters->get_value_at(op->outputContext());
            if (units_per_meter > 0.0) {
              lens_scale /= units_per_meter;
            }
          }
#endif
          add_knob_values(op, {"focal_point", "focusDistance", 1}, prim, values);
          values.push_back({prim,
                            "fStop",
                            {fstop_knob->get_value_at(op->outputContext()) * lens_scale},
                            {},
                            true});
        }
        const auto knob_value = [op](const char *name, int index, double fallback) {
          Knob *knob = op->knob(name);
          return knob ? knob->get_value_at(op->outputContext(), index) : fallback;
        };
        nkcycles::CameraWindow window;
        window.translate[0] = knob_value("win_translate", 0, 0.0);
        window.translate[1] = knob_value("win_translate", 1, 0.0);
        window.scale[0] = knob_value("win_scale", 0, 1.0);
        window.scale[1] = knob_value("win_scale", 1, 1.0);
        window.roll = knob_value("winroll", 0, 0.0);
        /* projection_mode: perspective, orthographic, uv, spherical. */
        const double focal = knob_value("focal", 0, 0.0);
        if (knob_value("projection_mode", 0, 0.0) == 1.0 && focal > 0.0) {
          window.ortho_width = knob_value("haperture", 0, 0.0) / focal;
        }
        options.camera_windows[prim] = window;
      }
    }
    /* Inputs not in use may be out of date. */
    for (int i = 0; i < op->inputs(); i++) {
      if (op->uses_input(i) > 0.0f) {
        collect_attributes(op->input(i), visited, options);
      }
    }
  }

  /* Build the scene and gather what the render needs from Nuke, once per hash. */
  void prepare(GeomOp *geo,
               const std::vector<GeomOp *> &shutter_geos,
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
    /* The input is made at the frame that FrameHold or TimeOffset nodes before us map to. */
    double frame = geo ? geo->outputContext().frame() : outputContext().frame();
    /* Motion blur: the input at frames evenly spread over the shutter, as FrameHold and
     * TimeOffset nodes map them (which may reverse them). */
    std::vector<double> shutter_frames;
    if (shutter_geos.size() > 1) {
      for (GeomOp *op : shutter_geos) {
        shutter_frames.push_back(op->outputContext().frame());
      }
      std::sort(shutter_frames.begin(), shutter_frames.end());
    }
    const double shutter = shutter_frames.empty() ? 0.0 :
                                                    shutter_frames.back() - shutter_frames.front();
    usg::StageRef stage;
    if (geo) {
      /* The sample times are frames, like the time codes of the stage. */
      fdk::TimeValueSet times{frame};
      times.insert(shutter_frames.begin(), shutter_frames.end());
#if kDDImageVersionInteger >= 161000
      GeometryProviderI::BuildStage(stage, usg::ArgSet(), OpGraphLocation(geo), times);
#else
      if (shutter == 0.0) {
        GeomOp::buildStage(stage, usg::ArgSet(), geo);
      }
      else {
        /* The engines write the shutter open, center and close. */
        GeomOp::buildStage(
            stage, usg::ArgSet(), GeomOpSet(shutter_geos.begin(), shutter_geos.end()), &times);
      }
#endif
    }
    if (!geo) {
      prepared->error = "No input scene";
    }
    else if (!stage || !stage->exportToString(prepared->usd_text, false)) {
      prepared->error = "Failed to export the input scene";
    }
    else {
      fix_array_types(prepared->usd_text);
      nkcycles::RenderOptions &options = prepared->options;
      options.width = format().width();
      options.height = format().height();
      options.samples = _samples;
      options.device = kDevices[_device];
      options.denoise = _denoise;
      options.depth_of_field = _depth_of_field;
      options.camera = _camera;
      options.holdouts = split_paths(_holdout);
      options.frame = frame;
      if (shutter > 0.0) {
        options.shutter = shutter;
        options.motion_steps = int(shutter_frames.size());
      }
      options.passes = passes;
      std::set<Op *> visited;
      collect_attributes(geo, visited, options);

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
  std::string _holdout;
  int _samples = 64;
  int _device = 0;
  bool _denoise = true;
  bool _depth_of_field = false;
  int _shutter_steps = 0;
  ShutterControls _shutter;
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

Op *build(Node *node)
{
  return new CyclesRender(node);
}

}  // namespace

const Op::Description CyclesRender::description(kClass, build);
