/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* CyclesRender: renders a scene of Nuke's USD-based 3D system with Cycles. */

#include "nuke/render.h"

#include <DDImage/Format.h>
#include <DDImage/GeomOp.h>
#include <DDImage/Knobs.h>
#include <DDImage/PlanarIop.h>
#include <usg/geom/Stage.h>

#include <algorithm>
#include <mutex>

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
  }

  void _validate(bool for_real) override
  {
    if (_formats.format()) {
      info_.format(*_formats.format());
      info_.full_size_format(*_formats.fullSizeFormat());
    }
    info_.set(format());
    info_.channels(Mask_RGBA);
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

  void renderStripe(ImagePlane &plane) override
  {
    plane.makeWritable();
    foreach(z, plane.channels())
    {
      plane.fillChannel(z, 0.0f);
    }

    GeomOp *geo = op_cast<GeomOp *>(Op::input(0));
    if (!geo) {
      return;
    }

    static std::once_flag init_flag;
    std::call_once(init_flag, [] { nkcycles::init(plugin_directory()); });

    usg::StageRef stage;
    GeomOp::buildStage(stage, usg::ArgSet(), geo);
    std::string usd_text;
    if (!stage || !stage->exportToString(usd_text, false)) {
      error("Failed to export the input scene");
      return;
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
    options.aborted = [this] { return aborted(); };

    std::vector<float> rgba;
    const std::string message = nkcycles::render(usd_text, options, rgba);
    if (!message.empty()) {
      if (!aborted()) {
        error("Cycles: %s", message.c_str());
      }
      return;
    }

    /* Copy the bottom-up Cycles image into the plane, clipped to its bounds. */
    const Box &bounds = plane.bounds();
    const int x0 = std::max(bounds.x(), 0), x1 = std::min(bounds.r(), options.width);
    const int y0 = std::max(bounds.y(), 0), y1 = std::min(bounds.t(), options.height);
    foreach(z, plane.channels())
    {
      const int chan = plane.chanNo(z);
      const int component = colourIndex(z);
      if (component > 3) {
        continue;
      }
      for (int y = y0; y < y1; y++) {
        const float *row = rgba.data() + size_t(y) * options.width * 4;
        for (int x = x0; x < x1; x++) {
          plane.writableAt(x, y, chan) = row[x * 4 + component];
        }
      }
    }
  }

 private:
  FormatPair _formats;
  std::string _camera;
  int _samples = 64;
  int _device = 0;
  bool _denoise = true;
};

Iop *build(Node *node)
{
  return new CyclesRender(node);
}

}  // namespace

const Iop::Description CyclesRender::description(kClass, "3D/CyclesRender", build);
