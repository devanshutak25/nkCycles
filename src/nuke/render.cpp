/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "nuke/render.h"

#include "hydra/file_reader.h"
#include "hydra/render_delegate.h"

#include "scene/camera.h"
#include "scene/integrator.h"
#include "scene/pass.h"
#include "scene/scene.h"
#include "session/buffers.h"
#include "session/output_driver.h"
#include "session/session.h"

#include "util/path.h"
#include "util/thread.h"
#include "util/unique_ptr.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <cstdlib>
#include <fstream>

namespace nkcycles {

using namespace ccl;
PXR_NAMESPACE_USING_DIRECTIVE

namespace {

/* Copies the combined pass of the finished image. */
class BufferOutputDriver : public OutputDriver {
 public:
  explicit BufferOutputDriver(std::vector<float> &rgba) : rgba_(rgba) {}

  void write_render_tile(const Tile &tile) override
  {
    /* Only the full image, no intermediate tiles. */
    if (!(tile.size == tile.full_size)) {
      return;
    }
    rgba_.resize(size_t(tile.size.x) * tile.size.y * 4);
    if (!tile.get_pass_pixels("combined", 4, rgba_.data())) {
      rgba_.clear();
    }
  }

 private:
  std::vector<float> &rgba_;
};

/* One render at a time: renders share the GPU, and Nuke may render several frames at once. */
thread_mutex render_mutex;

}  // namespace

void init(const std::string &root_path)
{
  path_init(root_path);
}

std::string render(const std::string &usd_text,
                   const RenderOptions &options,
                   std::vector<float> &rgba)
{
  rgba.clear();

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

  const thread_scoped_lock lock(render_mutex);

  HdRenderSettingsMap settings;
  settings[HD_CYCLES_NS::HdCyclesRenderSettingsTokens->device] = VtValue(options.device);
  SessionParams session_params = HD_CYCLES_NS::GetSessionParams(settings);
  session_params.background = true;
  session_params.samples = options.samples;

  /* shortcut: a new session (device, kernels, scene) per render, keep one per node for faster
   * re-renders. */
  const unique_ptr<Session> session = make_unique<Session>(session_params, SceneParams());
  session->set_output_driver(make_unique<BufferOutputDriver>(rgba));
  session->progress.set_update_callback([&] {
    if (options.aborted && options.aborted()) {
      session->progress.set_cancel("Cancelled");
    }
  });

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

  Pass *pass = scene->create_node<Pass>();
  pass->set_name(ustring("combined"));
  pass->set_type(PASS_COMBINED);

  BufferParams buffer_params;
  buffer_params.width = buffer_params.full_width = options.width;
  buffer_params.height = buffer_params.full_height = options.height;

  session->reset(session_params, buffer_params);
  session->start();
  session->wait();

  if (session->progress.get_cancel()) {
    rgba.clear();
    return session->progress.get_cancel_message();
  }
  if (session->progress.get_error()) {
    rgba.clear();
    return session->progress.get_error_message();
  }
  if (rgba.size() != size_t(options.width) * options.height * 4) {
    rgba.clear();
    return "Cycles produced no image";
  }
  return "";
}

}  // namespace nkcycles
