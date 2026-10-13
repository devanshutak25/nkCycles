/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Cycles rendering of a USD scene, kept free of Nuke headers so the Nuke node and
 * USD/Cycles headers never meet in one translation unit. */

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nkcycles {

/* A Cycles pass to read back, besides the "combined" pass that is always rendered. */
struct PassRequest {
  /* Unique name, the key in RenderResult::passes. Cryptomatte passes are named like Blender's
   * layers: "CryptoObject00", "CryptoMaterial01", ... */
  std::string name;
  /* Cycles pass type name: "depth", "normal", "diffuse_direct", "cryptomatte", ... */
  std::string type;
  /* Components per pixel. */
  int channels = 3;
};

/* A value Nuke keeps on a knob but does not write into the stage (light and camera knobs).
 * Set on the prim attribute when the stage has no authored value. */
struct AttributeValue {
  std::string prim;
  std::string attribute;
  std::vector<double> value;
  /* Value of an asset attribute. */
  std::string asset;
  /* Set even when the stage has a value. */
  bool force = false;
};

/* Knobs of a Nuke camera that the stage does not carry, or not the way Nuke uses them. */
struct CameraWindow {
  /* win_translate, win_scale and winroll (degrees), in film coordinates where the image width
   * spans -1 to 1 and y has the same scale. */
  double translate[2] = {0.0, 0.0};
  double scale[2] = {1.0, 1.0};
  double roll = 0.0;
  /* Width an orthographic camera sees in scene units, 0 for other cameras. */
  double ortho_width = 0.0;
};

/* A Nuke image used as texture. */
struct Image {
  int width = 0;
  int height = 0;
  /* Associated RGBA, bottom row first. */
  std::vector<float> pixels;
};

struct RenderOptions {
  int width = 0;
  int height = 0;
  int samples = 64;
  /* Cycles device: AUTO, OPTIX, CUDA or CPU. */
  std::string device = "AUTO";
  bool denoise = true;
  /* Off: no depth of field, whatever the camera's f-stop. */
  bool depth_of_field = false;
  /* Camera prim path, empty for the first camera in the stage. */
  std::string camera;
  double frame = 0.0;
  std::vector<PassRequest> passes;
  std::vector<AttributeValue> attributes;
  /* By the prim path of the camera node. */
  std::map<std::string, CameraWindow> camera_windows;
  /* Images of Nuke nodes by the asset path the scene refers to them with ("nkop:/NkRoot/..."). */
  std::map<std::string, std::shared_ptr<const Image>> images;
  /* Polled during the render, return true to cancel. */
  std::function<bool()> aborted;
};

struct RenderResult {
  /* Pixels of "combined" (RGBA) and of each requested pass, width * height * channels, bottom
   * row first. Like Nuke, depth is 1/z (0 for the background) and position is in stage units. */
  std::map<std::string, std::vector<float>> passes;
};

/* Directory holding the Cycles kernels ("lib" sub-folder). */
void init(const std::string &root_path);

/* Cryptomatte metadata of the specification ("cryptomatte/<id>/name", ".../manifest", ...) of a
 * USD layer given as text, with the names the render uses. types: 1 object, 2 material, 4 asset.
 * Does not render, so it is quick enough for Nuke's main thread. */
std::map<std::string, std::string> cryptomatte_metadata(const std::string &usd_text, int types);

/* Render a USD layer given as text. Returns an error message, or an empty string on success. */
std::string render(const std::string &usd_text,
                   const RenderOptions &options,
                   RenderResult &result);

}  // namespace nkcycles
