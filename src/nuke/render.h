/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Cycles rendering of a USD scene, kept free of Nuke headers so the Nuke node and
 * USD/Cycles headers never meet in one translation unit. */

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace nkcycles {

struct RenderOptions {
  int width = 0;
  int height = 0;
  int samples = 64;
  /* Cycles device: AUTO, OPTIX, CUDA or CPU. */
  std::string device = "AUTO";
  bool denoise = true;
  /* Camera prim path, empty for the first camera in the stage. */
  std::string camera;
  double frame = 0.0;
  /* Polled during the render, return true to cancel. */
  std::function<bool()> aborted;
};

/* Directory holding the Cycles kernels ("lib" sub-folder). */
void init(const std::string &root_path);

/* Render a USD layer given as text. Fills rgba with width * height RGBA pixels, bottom row
 * first. Returns an error message, or an empty string on success. */
std::string render(const std::string &usd_text,
                   const RenderOptions &options,
                   std::vector<float> &rgba);

}  // namespace nkcycles
