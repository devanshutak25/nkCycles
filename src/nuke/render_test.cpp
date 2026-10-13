/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Renders USD files like the Nuke node does, without Nuke, to test the rendering side with the
 * USD and libraries of a Nuke version. Several scenes are rendered in order in one process, so
 * the session kept between renders is tested too. */

#include "nuke/render.h"

#include <OpenImageIO/imageio.h>
#include <filesystem>

#include <pxr/base/plug/registry.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char *const kUsage =
    "Usage: nkcycles_render_test [options] <scene.usda> <out.exr> [<scene.usda> <out.exr> ...]\n"
    "Renders the scenes in order in one process, keeping the session between them. Options apply\n"
    "to the scenes after them.\n"
    "  --size <w> <h>              image size (default 320 180)\n"
    "  --samples <n>               samples (default 16)\n"
    "  --device <name>             AUTO, OPTIX, CUDA or CPU (default CPU)\n"
    "  --denoise <0|1>             denoise (default 0)\n"
    "  --frame <f>                 time code (default 1)\n"
    "  --motion <shutter> <steps>  motion blur over a shutter centered on the frame, odd steps\n"
    "  --camera <path>             camera prim (default the first)\n"
    "  --pass <name:type:n>        a Cycles pass with n components, e.g. depth:depth:1\n"
    "  --cryptomatte <type>        object, material or asset, with 3 layers\n"
    "  --attribute <prim:attr:v,..> a value for an attribute without authored value\n"
    "  --metadata                  print the cryptomatte metadata of each scene\n"
    "  --check-cryptomatte         fail when a rendered cryptomatte ID has no name in the "
    "manifest\n";

std::vector<std::string> split(const std::string &text, const char separator)
{
  std::vector<std::string> parts;
  std::stringstream stream(text);
  std::string part;
  while (std::getline(stream, part, separator)) {
    parts.push_back(part);
  }
  return parts;
}

/* Channel names of a pass: combined as RGBA, others prefixed with the pass name. */
std::vector<std::string> channel_names(const nkcycles::PassRequest &pass)
{
  if (pass.name == "combined") {
    return {"R", "G", "B", "A"};
  }
  if (pass.channels == 1) {
    return {pass.name + ".Z"};
  }
  const char *const components[] = {"R", "G", "B", "A"};
  std::vector<std::string> names;
  for (int i = 0; i < pass.channels; i++) {
    names.push_back(pass.name + "." + components[i]);
  }
  return names;
}

/* Write all passes into one EXR, top row first. */
bool write_exr(const std::string &path,
               const nkcycles::RenderOptions &options,
               const nkcycles::RenderResult &result)
{
  std::vector<nkcycles::PassRequest> passes = {{"combined", "combined", 4}};
  passes.insert(passes.end(), options.passes.begin(), options.passes.end());

  std::vector<std::string> names;
  std::vector<const nkcycles::PassRequest *> sources;
  for (const nkcycles::PassRequest &pass : passes) {
    if (result.passes.count(pass.name)) {
      for (const std::string &name : channel_names(pass)) {
        names.push_back(name);
      }
      sources.push_back(&pass);
    }
  }

  const int width = options.width, height = options.height;
  const int num_channels = int(names.size());
  std::vector<float> pixels(size_t(width) * height * num_channels);
  int channel = 0;
  for (const nkcycles::PassRequest *pass : sources) {
    const std::vector<float> &data = result.passes.at(pass->name);
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        for (int c = 0; c < pass->channels; c++) {
          pixels[(size_t(height - 1 - y) * width + x) * num_channels + channel + c] =
              data[(size_t(y) * width + x) * pass->channels + c];
        }
      }
    }
    channel += pass->channels;
  }

  OIIO::ImageSpec spec(width, height, num_channels, OIIO::TypeDesc::FLOAT);
  spec.channelnames = names;
  spec.alpha_channel = 3;
  std::unique_ptr<OIIO::ImageOutput> out = OIIO::ImageOutput::create(path);
  if (!out || !out->open(path, spec) || !out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) {
    fprintf(stderr, "Failed to write %s\n", path.c_str());
    return false;
  }
  out->close();
  return true;
}

/* Count the IDs in the cryptomatte passes that have no name in the manifest of their layer. */
int unnamed_cryptomatte_ids(const nkcycles::RenderOptions &options,
                            const nkcycles::RenderResult &result,
                            const std::map<std::string, std::string> &metadata)
{
  /* Manifest IDs of each layer name, from {"name":"hex",...}. */
  std::map<std::string, std::set<uint32_t>> manifest_ids;
  for (const auto &[key, value] : metadata) {
    if (key.size() > 5 && key.compare(key.size() - 5, 5, "/name") == 0) {
      const auto manifest = metadata.find(key.substr(0, key.size() - 4) + "manifest");
      if (manifest == metadata.end()) {
        continue;
      }
      std::set<uint32_t> &ids = manifest_ids[value];
      const std::string &json = manifest->second;
      for (size_t colon = json.find("\":\""); colon != std::string::npos;
           colon = json.find("\":\"", colon + 1))
      {
        ids.insert(uint32_t(std::stoul(json.substr(colon + 3, 8), nullptr, 16)));
      }
    }
  }

  int unnamed = 0;
  for (const nkcycles::PassRequest &pass : options.passes) {
    const auto pixels = result.passes.find(pass.name);
    if (pass.type != "cryptomatte" || pixels == result.passes.end()) {
      continue;
    }
    const std::string layer = pass.name.substr(0, pass.name.size() - 2);
    std::set<uint32_t> missing;
    for (size_t i = 0; i + 3 < pixels->second.size(); i += 4) {
      for (int rank = 0; rank < 2; rank++) {
        const float id = pixels->second[i + rank * 2], coverage = pixels->second[i + rank * 2 + 1];
        uint32_t bits;
        memcpy(&bits, &id, sizeof(bits));
        if (coverage > 0.0f && !manifest_ids[layer].count(bits)) {
          missing.insert(bits);
        }
      }
    }
    for (const uint32_t bits : missing) {
      printf("UNNAMED %s %08x\n", pass.name.c_str(), bits);
    }
    unnamed += int(missing.size());
  }
  return unnamed;
}

}  // namespace

int main(int argc, char **argv)
{
  nkcycles::RenderOptions options;
  options.width = 320;
  options.height = 180;
  options.samples = 16;
  options.device = "CPU";
  options.denoise = false;
  options.frame = 1.0;
  bool print_metadata = false;
  bool check_cryptomatte = false;
  struct Job {
    std::string scene, output;
    nkcycles::RenderOptions options;
    int cryptomatte_types;
  };
  std::vector<Job> jobs;
  int cryptomatte_types = 0;

  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "Missing value for %s\n%s", arg.c_str(), kUsage);
        exit(2);
      }
      return argv[++i];
    };
    if (arg == "--size") {
      options.width = std::stoi(value());
      options.height = std::stoi(value());
    }
    else if (arg == "--samples") {
      options.samples = std::stoi(value());
    }
    else if (arg == "--device") {
      options.device = value();
    }
    else if (arg == "--denoise") {
      options.denoise = value() != "0";
    }
    else if (arg == "--frame") {
      options.frame = std::stod(value());
    }
    else if (arg == "--motion") {
      options.shutter = std::stod(value());
      options.motion_steps = std::stoi(value());
    }
    else if (arg == "--camera") {
      options.camera = value();
    }
    else if (arg == "--pass") {
      const std::vector<std::string> parts = split(value(), ':');
      if (parts.size() != 3) {
        fprintf(stderr, "Pass must be name:type:components\n");
        return 2;
      }
      options.passes.push_back({parts[0], parts[1], std::stoi(parts[2])});
    }
    else if (arg == "--cryptomatte") {
      /* Passes as the node makes them: 3 layers of each type, in this order. */
      const std::string type = value();
      const char *const types[] = {"object", "material", "asset"};
      const char *const layers[] = {"CryptoObject", "CryptoMaterial", "CryptoAsset"};
      for (int t = 0; t < 3; t++) {
        if (type == types[t]) {
          cryptomatte_types |= 1 << t;
        }
      }
      options.passes.erase(std::remove_if(options.passes.begin(),
                                          options.passes.end(),
                                          [](const nkcycles::PassRequest &pass) {
                                            return pass.type == "cryptomatte";
                                          }),
                           options.passes.end());
      for (int t = 0; t < 3; t++) {
        for (int level = 0; (cryptomatte_types & (1 << t)) && level < 3; level++) {
          char name[32];
          snprintf(name, sizeof(name), "%s%02d", layers[t], level);
          options.passes.push_back({name, "cryptomatte", 4});
        }
      }
    }
    else if (arg == "--attribute") {
      const std::string text = value();
      /* Prim paths have no ':', attribute names can ("inputs:intensity"). */
      const size_t first = text.find(':'), last = text.rfind(':');
      if (first == std::string::npos || last == first) {
        fprintf(stderr, "Attribute must be prim:attribute:values\n");
        return 2;
      }
      nkcycles::AttributeValue attribute{
          text.substr(0, first), text.substr(first + 1, last - first - 1), {}};
      for (const std::string &number : split(text.substr(last + 1), ',')) {
        attribute.value.push_back(std::stod(number));
      }
      options.attributes.push_back(attribute);
    }
    else if (arg == "--metadata") {
      print_metadata = true;
    }
    else if (arg == "--check-cryptomatte") {
      check_cryptomatte = true;
    }
    else if (arg.rfind("--", 0) == 0) {
      fprintf(stderr, "Unknown option %s\n%s", arg.c_str(), kUsage);
      return 2;
    }
    else {
      const std::string output = value();
      jobs.push_back({arg, output, options, cryptomatte_types});
    }
  }
  if (jobs.empty()) {
    fprintf(stderr, "%s", kUsage);
    return 2;
  }

  /* Kernels and USD plug-ins next to the program. */
  const std::string root = std::filesystem::absolute(argv[0]).parent_path().string();
  nkcycles::init(root);
  PXR_NS::PlugRegistry::GetInstance().RegisterPlugins(root + "/usd");

  int failures = 0;
  for (const Job &job : jobs) {
    const std::string &scene = job.scene, &output = job.output;
    const nkcycles::RenderOptions &options = job.options;
    std::ifstream file(scene, std::ios::binary);
    if (!file) {
      fprintf(stderr, "Can not read %s\n", scene.c_str());
      return 1;
    }
    std::stringstream text;
    text << file.rdbuf();

    nkcycles::RenderResult result;
    const std::string message = nkcycles::render(text.str(), options, result);
    if (!message.empty()) {
      fprintf(stderr, "%s: %s\n", scene.c_str(), message.c_str());
      failures++;
      continue;
    }
    /* After the render, which knows the names Hydra gives to instanced prims. */
    if (job.cryptomatte_types && (print_metadata || check_cryptomatte)) {
      const std::map<std::string, std::string> metadata = nkcycles::cryptomatte_metadata(
          text.str(), job.cryptomatte_types);
      for (const auto &[key, value] : metadata) {
        if (print_metadata) {
          printf("METADATA %s %s\n", key.c_str(), value.c_str());
        }
      }
      if (check_cryptomatte && unnamed_cryptomatte_ids(options, result, metadata)) {
        fprintf(stderr, "%s: cryptomatte IDs without name\n", scene.c_str());
        failures++;
      }
    }
    if (!write_exr(output, options, result)) {
      failures++;
      continue;
    }
    printf("RENDERED %s -> %s\n", scene.c_str(), output.c_str());
    fflush(stdout);
  }
  return failures ? 1 : 0;
}
