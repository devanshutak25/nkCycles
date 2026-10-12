/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/*
 * Standalone tool that links against the compiled Cycles libraries,
 * queries the NodeType registry for all registered shader nodes,
 * and writes a JSON description to shader_nodes.json.
 *
 * Build (from build/):
 *   cmake --build . --target shaders_node_dump
 *
 * Or compile manually linking against cycles libraries.
 *
 * Usage: ./shaders_node_dump
 * Output: shader_nodes.json (in the current working directory)
 */

#include "graph/node_type.h"
#include "scene/shader_nodes.h"
#include "graph/node_enum.h"
#include "util/log.h"
#include "util/path.h"
#include "util/types_float3.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

CCL_NAMESPACE_BEGIN

static const char *OUTPUT_PATH = "shader_nodes.json";

/* ------------------------------------------------------------------ */
/* JSON helpers                                                        */
/* ------------------------------------------------------------------ */

static std::string json_escape(const std::string &s)
{
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

static std::string title_case(const std::string &s)
{
  std::string out;
  bool cap_next = true;
  for (char c : s) {
    if (c == '_' || c == ' ') {
      out += ' ';
      cap_next = true;
    }
    else if (cap_next) {
      out += (char)std::toupper((unsigned char)c);
      cap_next = false;
    }
    else {
      out += c;
    }
  }
  return out;
}

static std::string socket_type_str(SocketType::Type type)
{
  switch (type) {
    case SocketType::BOOLEAN:
      return "boolean";
    case SocketType::FLOAT:
      return "float";
    case SocketType::INT:
      return "int";
    case SocketType::UINT:
      return "uint";
    case SocketType::UINT64:
      return "uint64";
    case SocketType::COLOR:
      return "color";
    case SocketType::VECTOR:
      return "vector";
    case SocketType::POINT:
      return "point";
    case SocketType::NORMAL:
      return "normal";
    case SocketType::POINT2:
      return "point2";
    case SocketType::CLOSURE:
      return "closure";
    case SocketType::STRING:
      return "string";
    case SocketType::ENUM:
      return "menu";
    case SocketType::TRANSFORM:
      return "matrix";
    case SocketType::NODE:
      return "node";
    default:
      return "float";
  }
}

/* ------------------------------------------------------------------ */
/* Min/max heuristic (matching osl2hda.py patterns)                    */
/* ------------------------------------------------------------------ */

struct MinMax {
  bool has_min = false;
  bool has_max = false;
  float min_val = 0.0f;
  float max_val = 1.0f;
  bool soft_min = false;
  bool soft_max = false;
};

static MinMax infer_range(const std::string &name)
{
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

  MinMax r;

  /* Parameters that are [0, 1] by nature */
  if (lower.find("roughness") != std::string::npos ||
      lower.find("metallic") != std::string::npos ||
      lower.find("alpha") != std::string::npos ||
      lower.find("weight") != std::string::npos ||
      lower.find("specular") != std::string::npos ||
      lower.find("coat") != std::string::npos ||
      lower.find("sheen") != std::string::npos ||
      lower.find("transmission") != std::string::npos ||
      lower.find("subsurface") != std::string::npos ||
      lower.find("fac") != std::string::npos ||
      lower.find("blend") != std::string::npos ||
      lower.find("factor") != std::string::npos ||
      lower.find("anisotropy") != std::string::npos ||
      lower.find("rotation") != std::string::npos) {
    r.has_min = true;
    r.has_max = true;
    r.min_val = 0.0f;
    r.max_val = 1.0f;
    return r;
  }

  /* Parameters that are >= 0 */
  if (lower.find("ior") != std::string::npos ||
      lower.find("scale") != std::string::npos ||
      lower.find("radius") != std::string::npos ||
      lower.find("thickness") != std::string::npos ||
      lower.find("strength") != std::string::npos ||
      lower.find("samples") != std::string::npos ||
      lower.find("distance") != std::string::npos ||
      lower.find("density") != std::string::npos ||
      lower.find("intensity") != std::string::npos ||
      lower.find("size") != std::string::npos ||
      lower.find("detail") != std::string::npos ||
      lower.find("distortion") != std::string::npos ||
      lower.find("wavelength") != std::string::npos ||
      lower.find("temperature") != std::string::npos ||
      lower.find("height") != std::string::npos ||
      lower.find("depth") != std::string::npos ||
      lower.find("brightness") != std::string::npos) {
    r.has_min = true;
    r.has_max = false;
    r.min_val = 0.0f;
    return r;
  }

  /* Angles in radians: typically [0, 2*pi] or similar */
  if (lower.find("angle") != std::string::npos ||
      lower.find("elevation") != std::string::npos ||
      lower.find("rotation") != std::string::npos) {
    r.has_min = true;
    r.has_max = true;
    r.min_val = 0.0f;
    r.max_val = 6.28318f;
    return r;
  }

  return r;
}

/* ------------------------------------------------------------------ */
/* Socket default value extraction                                     */
/* ------------------------------------------------------------------ */

static std::string extract_default(const SocketType &socket)
{
  if (socket.default_value == nullptr)
    return "";

  const void *dv = socket.default_value;

  switch (socket.type) {
    case SocketType::BOOLEAN: {
      bool val = *(const bool *)dv;
      return val ? "true" : "false";
    }
    case SocketType::FLOAT: {
      float val = *(const float *)dv;
      /* Format to avoid trailing zeros */
      char buf[64];
      snprintf(buf, sizeof(buf), "%g", val);
      return buf;
    }
    case SocketType::INT:
    case SocketType::UINT:
    case SocketType::ENUM: {
      int val = *(const int *)dv;
      return std::to_string(val);
    }
    case SocketType::UINT64: {
      uint64_t val = *(const uint64_t *)dv;
      return std::to_string(val);
    }
    case SocketType::COLOR:
    case SocketType::VECTOR:
    case SocketType::POINT:
    case SocketType::NORMAL: {
      const float3 *val = (const float3 *)dv;
      char buf[128];
      snprintf(buf, sizeof(buf), "[%g, %g, %g]", (double)val->x, (double)val->y, (double)val->z);
      return buf;
    }
    case SocketType::POINT2: {
      const float2 *val = (const float2 *)dv;
      char buf[64];
      snprintf(buf, sizeof(buf), "[%g, %g]", (double)val->x, (double)val->y);
      return buf;
    }
    case SocketType::STRING: {
      const ustring *val = (const ustring *)dv;
      const char *str = val->c_str();
      if (!str)
        return "\"\"";
      return "\"" + json_escape(str) + "\"";
    }
    case SocketType::TRANSFORM: {
      return "[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]";
    }
    default:
      return "";
  }
}

/* Extract enum token string from the enum_values and an integer default. */
static std::string enum_default_token(const NodeEnum *enum_values, int int_default)
{
  if (!enum_values)
    return std::to_string(int_default);

  ustring token = (*enum_values)[int_default];
  if (token.empty())
    return std::to_string(int_default);

  return "\"" + json_escape(token.c_str()) + "\"";
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

CCL_NAMESPACE_END

using namespace ccl;

int main()
{
  log_init(nullptr);
  log_level_set(LOG_LEVEL_WARNING);
  path_init();

  /* Reference a shader node so the linker keeps the object file with the shader node types,
   * which register themselves on static initialization. */
  PrincipledBsdfNode::get_node_type();

  /* Get all registered node type names */
  vector<ustring> names = NodeType::type_names();

  /* Filter to only SHADER type nodes */
  vector<const NodeType *> shader_nodes;
  for (const ustring &name : names) {
    const NodeType *nt = NodeType::find(name);
    if (nt && nt->type == NodeType::SHADER) {
      shader_nodes.push_back(nt);
    }
  }

  /* Sort by name for consistent output */
  std::sort(shader_nodes.begin(), shader_nodes.end(), [](const NodeType *a, const NodeType *b) {
    return a->name.c_str() < b->name.c_str();
  });

  /* Write JSON */
  std::ofstream out(OUTPUT_PATH);
  if (!out.is_open()) {
    std::cerr << "Error: cannot write to " << OUTPUT_PATH << std::endl;
    return 1;
  }

  out << "{\n  \"nodes\": [\n";

  for (size_t ni = 0; ni < shader_nodes.size(); ni++) {
    const NodeType *nt = shader_nodes[ni];
    std::string node_name = nt->name.c_str();
    std::string label = title_case(node_name);

    out << "    {\n";
    out << "      \"name\": \"" << json_escape(node_name) << "\",\n";
    out << "      \"label\": \"" << json_escape(label) << "\",\n";

    /* Inputs */
    out << "      \"inputs\": [\n";
    bool first_input = true;
    for (const SocketType &socket : nt->inputs) {
      /* Skip internal sockets */
      if (socket.flags & SocketType::SVM_INTERNAL)
        continue;
      if (socket.flags & SocketType::OSL_INTERNAL)
        continue;

      std::string type_str = socket_type_str(socket.type);
      std::string sname = socket.name.c_str();
      std::string sui_name = socket.ui_name.c_str();
      if (sui_name.empty())
        sui_name = sname;

      if (!first_input)
        out << ",\n";
      first_input = false;

      out << "        {\n";
      out << "          \"name\": \"" << json_escape(sname) << "\",\n";
      out << "          \"type\": \"" << json_escape(type_str) << "\",\n";
      out << "          \"ui_name\": \"" << json_escape(sui_name) << "\"";

      /* Default value */
      if (socket.type == SocketType::ENUM) {
        /* For enums, extract the int default and reverse-map to token */
        if (socket.default_value) {
          int int_default = *(const int *)socket.default_value;
          std::string token = enum_default_token(socket.enum_values, int_default);
          out << ",\n          \"default\": " << token;
        }
      }
      else {
        std::string def = extract_default(socket);
        if (!def.empty()) {
          out << ",\n          \"default\": " << def;
        }
      }

      /* Enum values */
      if (socket.type == SocketType::ENUM && socket.enum_values) {
        out << ",\n          \"enum_values\": {";
        bool first_ev = true;
        for (auto it = socket.enum_values->begin(); it != socket.enum_values->end(); ++it) {
          if (!first_ev)
            out << ", ";
          first_ev = false;
          std::string token = it->first.c_str();
          std::string display = title_case(token);
          out << "\"" << json_escape(token) << "\": \"" << json_escape(display) << "\"";
        }
        out << "}";
      }

      /* Min/max range inference */
      if (type_str == "float" && socket.type != SocketType::ENUM) {
        MinMax mm = infer_range(sname);
        if (mm.has_min) {
          char buf[32];
          snprintf(buf, sizeof(buf), "%g", (double)mm.min_val);
          out << ",\n          \"min\": " << buf;
        }
        if (mm.has_max) {
          char buf[32];
          snprintf(buf, sizeof(buf), "%g", (double)mm.max_val);
          out << ",\n          \"max\": " << buf;
        }
      }

      out << "\n        }";
    }
    out << "\n      ],\n";

    /* Outputs */
    out << "      \"outputs\": [\n";
    bool first_output = true;
    for (const SocketType &socket : nt->outputs) {
      std::string type_str = socket_type_str(socket.type);
      std::string sname = socket.name.c_str();
      std::string sui_name = socket.ui_name.c_str();
      if (sui_name.empty())
        sui_name = sname;

      if (!first_output)
        out << ",\n";
      first_output = false;

      out << "        {\n";
      out << "          \"name\": \"" << json_escape(sname) << "\",\n";
      out << "          \"type\": \"" << json_escape(type_str) << "\",\n";
      out << "          \"ui_name\": \"" << json_escape(sui_name) << "\"";
      out << "\n        }";
    }
    out << "\n      ]\n";

    out << "    }";
    if (ni + 1 < shader_nodes.size())
      out << ",";
    out << "\n";
  }

  out << "  ]\n}\n";
  out.close();

  std::cout << "Extracted " << shader_nodes.size() << " shader nodes to " << OUTPUT_PATH
            << std::endl;
  return 0;
}
