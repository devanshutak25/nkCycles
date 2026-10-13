/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/material.h"
#ifdef WITH_HYDRA_MATERIALX
#  include "hydra/materialx.h"
#endif
#include "hydra/node_util.h"
#include "hydra/session.h"
#include "hydra/util.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "util/colorspace.h"
#include "util/path.h"
#include "util/string.h"

#include <pxr/base/js/json.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialConnectionSchema.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/sceneDelegate.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Normalize a material network node name to a full SdfPath. The schema may
 * provide either a full path or a bare identifier. */
static SdfPath MaterialNodeNameToSdfPath(const TfToken &nodeName)
{
  const std::string &s = nodeName.GetString();
  if (s.empty()) {
    return SdfPath::EmptyPath();
  }
  if (s[0] == '/' && SdfPath::IsValidPathString(s)) {
    return SdfPath(s);
  }
  return SdfPath::AbsoluteRootPath().AppendChild(nodeName);
}

/* Houdini's AOV Output node has a "shader" input passed through to its "out" output, see
 * shader2hda.py. */
static const TfToken kAovPassThroughInput("shader");

static bool IsAovPassThrough(const ShaderNode *node, const TfToken &socketName)
{
  return node && node->type->name == "aov_output" &&
         (socketName == kAovPassThroughInput || socketName == "out");
}

/* Default values for Cycles shader node sockets that differ from the Cycles socket defaults,
 * read from "shader_defaults.json" in the resource directory. Host integrations write this
 * when their shader node UI shows other defaults, e.g. the Houdini shader nodes show Blender's
 * defaults. Hosts only write values that differ from the UI default to USD, so the render only
 * matches the UI when these are applied first. */
class ShaderNodeDefaults {
  using Defaults = std::vector<std::pair<ustring, VtValue>>;

 public:
  ShaderNodeDefaults()
  {
    const string filepath = path_get("shader_defaults.json");
    if (!path_exists(filepath)) {
      return;
    }
    string text;
    if (!path_read_text(filepath, text)) {
      TF_WARN("Could not read shader node defaults from '%s'", filepath.c_str());
      return;
    }
    JsParseError error;
    const JsValue root = JsParseString(text, &error);
    if (!root.IsObject()) {
      TF_WARN("Could not parse shader node defaults from '%s', line %u: %s",
              filepath.c_str(),
              error.line,
              error.reason.c_str());
      return;
    }
    for (const auto &[nodeName, sockets] : root.GetJsObject()) {
      if (!sockets.IsObject()) {
        continue;
      }
      Defaults &defaults = _nodes[nodeName];
      for (const auto &[socketName, value] : sockets.GetJsObject()) {
        VtValue converted = ConvertValue(value);
        if (!converted.IsEmpty()) {
          defaults.emplace_back(ustring(socketName), std::move(converted));
        }
      }
    }
  }

  void Apply(ShaderNode *node, const string &nodeType) const
  {
    const auto it = _nodes.find(nodeType);
    if (it == _nodes.end()) {
      return;
    }
    for (const auto &[socketName, value] : it->second) {
      if (const SocketType *socket = node->type->find_input(socketName)) {
        SetNodeValue(node, *socket, value);
      }
    }
  }

 private:
  static VtValue ConvertValue(const JsValue &value)
  {
    if (value.IsBool()) {
      return VtValue(value.GetBool());
    }
    if (value.IsInt()) {
      return VtValue(value.GetInt());
    }
    if (value.IsReal()) {
      return VtValue(float(value.GetReal()));
    }
    if (value.IsString()) {
      return VtValue(value.GetString());
    }
    if (value.IsArrayOf<double>()) {
      const std::vector<double> array = value.GetArrayOf<double>();
      if (array.size() == 2) {
        return VtValue(GfVec2f(array[0], array[1]));
      }
      if (array.size() == 3) {
        return VtValue(GfVec3f(array[0], array[1], array[2]));
      }
    }
    return VtValue();
  }

  std::unordered_map<string, Defaults> _nodes;
};

static const ShaderNodeDefaults &GetShaderNodeDefaults()
{
  static const ShaderNodeDefaults defaults;
  return defaults;
}

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(CyclesMaterialTokens,
    (cycles)
    ((cyclesSurface, "cycles:surface"))
    ((cyclesDisplacement, "cycles:displacement"))
    ((cyclesVolume, "cycles:volume"))
    (UsdPreviewSurface)
    (UsdUVTexture)
    (UsdPrimvarReader_float)
    (UsdPrimvarReader_float2)
    (UsdPrimvarReader_float3)
    (UsdPrimvarReader_float4)
    (UsdPrimvarReader_int)
    (UsdTransform2d)
    (NukeDefaultSurface)
    (normal)
    (a)
    (rgb)
    (r)
    (g)
    (b)
    (result)
    (st)
    (wrapS)
    (wrapT)
    (periodic)
);
// clang-format on

/* Simple class to handle remapping of USDPreviewSurface nodes and parameters to Cycles
 * equivalents. */
class UsdToCyclesMapping {
  using ParamMap = std::unordered_map<TfToken, ustring, TfToken::HashFunctor>;
  /* Values of Cycles inputs where the USD node's fallback differs from the Cycles default. */
  using Defaults = std::vector<std::pair<ustring, VtValue>>;

 public:
  UsdToCyclesMapping(const char *nodeType, ParamMap paramMap, Defaults defaults = {})
      : _nodeType(nodeType), _paramMap(std::move(paramMap)), _defaults(std::move(defaults))
  {
  }

  ustring nodeType() const
  {
    return _nodeType;
  }

  void applyDefaults(Node *node) const
  {
    for (const auto &[name, value] : _defaults) {
      if (const SocketType *input = node->type->find_input(name)) {
        SetNodeValue(node, *input, value);
      }
    }
  }

  bool hasParameterMapping(const TfToken &name) const
  {
    return _paramMap.find(name) != _paramMap.end();
  }

  virtual std::string parameterName(const TfToken &name,
                                    const ShaderInput *inputConnection,
                                    VtValue * /*value*/ = nullptr) const
  {
    /* UsdNode.name -> Node.input. These all follow a simple pattern that we can just
     * remap based on the name or 'Node.input' type. */
    if (inputConnection) {
      if (name == CyclesMaterialTokens->a || name == "opacity") {
        return "alpha";
      }
      if (name == CyclesMaterialTokens->rgb || name == "rgba") {
        return "color";
      }
      /* TODO: Is there a better mapping than 'color'? */
      if (name == CyclesMaterialTokens->r || name == CyclesMaterialTokens->g ||
          name == CyclesMaterialTokens->b)
      {
        return "color";
      }

      if (name == CyclesMaterialTokens->result) {
        switch (inputConnection->socket_type.type) {
          case SocketType::BOOLEAN:
          case SocketType::FLOAT:
          case SocketType::INT:
          case SocketType::UINT:
            return "alpha";
          case SocketType::COLOR:
          case SocketType::VECTOR:
          case SocketType::POINT:
          case SocketType::NORMAL:
          default:
            return "color";
        }
      }
    }

    /* Simple mapping case */
    const auto it = _paramMap.find(name);
    return it != _paramMap.end() ? it->second.string() : name.GetString();
  }

 private:
  const ustring _nodeType;
  ParamMap _paramMap;
  Defaults _defaults;
};

class UsdToCyclesTexture : public UsdToCyclesMapping {
 public:
  using UsdToCyclesMapping::UsdToCyclesMapping;

  std::string parameterName(const TfToken &name,
                            const ShaderInput *inputConnection,
                            VtValue *value) const override
  {
    if (value) {
      /* Remap UsdUVTexture.wrapS and UsdUVTexture.wrapT to cycles_image_texture.extension. */
      if (name == CyclesMaterialTokens->wrapS || name == CyclesMaterialTokens->wrapT) {
        const std::string valueString = VtValue::Cast<std::string>(*value).Get<std::string>();

        /* A value of 'repeat' in USD is equivalent to 'periodic' in Cycles. */
        if (valueString == "repeat") {
          *value = VtValue(CyclesMaterialTokens->periodic);
        }

        return "extension";
      }
    }

    return UsdToCyclesMapping::parameterName(name, inputConnection, value);
  }
};

namespace {

class UsdToCycles {
  const UsdToCyclesMapping UsdPreviewSurface = {
      "principled_bsdf",
      {
          {TfToken("diffuseColor"), ustring("base_color")},
          {TfToken("emissiveColor"), ustring("emission_color")},
          {TfToken("specularColor"), ustring("specular_tint")},
          {TfToken("clearcoatRoughness"), ustring("coat_roughness")},
          {TfToken("opacity"), ustring("alpha")},
          /* opacityThreshold */
          /* occlusion */
          /* displacement */
      },
      /* Fallbacks of UsdPreviewSurface inputs that are not authored. */
      {
          {ustring("base_color"), VtValue(GfVec3f(0.18f))},
          {ustring("emission_color"), VtValue(GfVec3f(0.0f))},
          {ustring("emission_strength"), VtValue(1.0f)},
          {ustring("coat_roughness"), VtValue(0.01f)},
      }};
  const UsdToCyclesTexture UsdUVTexture = {
      "image_texture",
      {
          {CyclesMaterialTokens->st, ustring("vector")},
          {CyclesMaterialTokens->wrapS, ustring("extension")},
          {CyclesMaterialTokens->wrapT, ustring("extension")},
          {TfToken("file"), ustring("filename")},
          {TfToken("sourceColorSpace"), ustring("colorspace")},
      }};
  const UsdToCyclesMapping UsdPrimvarReader = {"attribute",
                                               {{TfToken("varname"), ustring("attribute")}}};

 public:
  const UsdToCyclesMapping *findUsd(const TfToken &usdNodeType)
  {
    if (usdNodeType == CyclesMaterialTokens->UsdPreviewSurface) {
      return &UsdPreviewSurface;
    }
    if (usdNodeType == CyclesMaterialTokens->UsdUVTexture) {
      return &UsdUVTexture;
    }
    if (usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_float ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_float2 ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_float3 ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_float4 ||
        usdNodeType == CyclesMaterialTokens->UsdPrimvarReader_int)
    {
      return &UsdPrimvarReader;
    }

    return nullptr;
  }
  const UsdToCyclesMapping *findCycles(const ustring & /*cyclesNodeType*/)
  {
    return nullptr;
  }
};
TfStaticData<UsdToCycles> sUsdToCyles;

/* Socket name for comparison, ignoring case, spaces, underscores and dots. Houdini parameter names
 * can't contain dots, so e.g. "tex_mapping.scale" becomes "tex_mapping_scale". */
static std::string NormalizeName(const std::string &name)
{
  std::string result;
  for (char c : name) {
    if (c != '_' && c != ' ' && c != '.') {
      result += std::tolower(static_cast<unsigned char>(c));
    }
  }
  if (result == "vectorin" || result == "vectorout") {
    return "vector";
  }
  return result;
}

/* Find a socket by either its identifier (e.g. "emission", as used by the Houdini shader nodes)
 * or its UI name (e.g. "Emission", as exported by Blender). Identifiers take precedence, as UI
 * names are ambiguous: e.g. texture nodes have both a "scale" input and a "tex_mapping.scale"
 * input with UI name "Scale". */
static bool SocketNameMatches(const SocketType &socket,
                              const std::string &normName,
                              const bool use_ui_name)
{
  return NormalizeName(use_ui_name ? socket.ui_name.string() : socket.name.string()) == normName;
}

static const SocketType *FindSocketType(const NodeType *type, const std::string &name)
{
  const std::string normName = NormalizeName(name);
  for (const bool use_ui_name : {false, true}) {
    for (const SocketType &socket : type->inputs) {
      if (SocketNameMatches(socket, normName, use_ui_name)) {
        return &socket;
      }
    }
  }
  return nullptr;
}

static ShaderInput *FindInput(ShaderNode *node, const std::string &name)
{
  const std::string normName = NormalizeName(name);
  for (const bool use_ui_name : {false, true}) {
    for (ShaderInput *in : node->inputs) {
      if (SocketNameMatches(in->socket_type, normName, use_ui_name)) {
        return in;
      }
    }
  }
  return nullptr;
}

static ShaderOutput *FindOutput(ShaderNode *node, const std::string &name)
{
  const std::string normName = NormalizeName(name);
  for (const bool use_ui_name : {false, true}) {
    for (ShaderOutput *out : node->outputs) {
      if (SocketNameMatches(out->socket_type, normName, use_ui_name)) {
        return out;
      }
    }
  }
  return nullptr;
}

}  // namespace

static void AddInstanceIdAOV(ShaderGraph *graph)
{
  /* Instance AOV is provided for every network, to make picking work. */
  const ustring instanceId(HdAovTokens->instanceId.GetString());

  OutputAOVNode *aovNode = graph->create_node<OutputAOVNode>();
  aovNode->set_name(instanceId);

  AttributeNode *instanceIdNode = graph->create_node<AttributeNode>();
  instanceIdNode->set_attribute(instanceId);

  graph->connect(instanceIdNode->output("Fac"), aovNode->input("Value"));
}

HdCyclesMaterial::HdCyclesMaterial(const SdfPath &sprimId) : HdMaterial(sprimId) {}

HdCyclesMaterial::~HdCyclesMaterial() = default;

HdDirtyBits HdCyclesMaterial::GetInitialDirtyBitsMask() const
{
  return DirtyBits::DirtyResource | DirtyBits::DirtyParams;
}

void HdCyclesMaterial::Sync(HdSceneDelegate *sceneDelegate,
                            HdRenderParam *renderParam,
                            HdDirtyBits *dirtyBits)
{
  if (*dirtyBits == DirtyBits::Clean) {
    return;
  }

  const bool dirtyParams = (*dirtyBits & DirtyBits::DirtyParams);
  const bool dirtyResource = (*dirtyBits & DirtyBits::DirtyResource);

  const SdfPath &id = GetId();

  if (dirtyResource || dirtyParams) {
    const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);
    const HdContainerDataSourceHandle &primDs = prim.dataSource;

    HdCyclesMaterialSchema matSchema = HdMaterialSchema::GetFromParent(primDs);
    /* Prefer cycles network if it exists, then MaterialX, otherwise use universal network. */
    HdMaterialNetworkSchema network = matSchema.GetMaterialNetwork(CyclesMaterialTokens->cycles);
#ifdef WITH_HYDRA_MATERIALX
    for (const TfToken &context : MaterialXRenderContexts()) {
      if (!network) {
        network = matSchema.GetMaterialNetwork(context);
      }
    }
#endif
    if (!network) {
      network = matSchema.GetMaterialNetwork();
    }
#ifdef WITH_HYDRA_MATERIALX
    const bool isMaterialX = network && IsMaterialXNetwork(network);
#endif

    const SceneLock lock(renderParam);

    /* Always build a new graph: Cycles simplifies the graph of a shader when compiling it,
     * deleting nodes, so the nodes of the last graph can not be updated in place. */
    {
      unique_ptr<ShaderGraph> graph;
      if (!network) {
        TF_WARN("Could not get a material network for %s.", id.GetText());
      }
      else {
#ifdef WITH_HYDRA_MATERIALX
        if (isMaterialX) {
          /* Always rebuilt, parameter updates need the whole network. */
          _nodes.clear();
          graph = BuildMaterialXShaderGraph(id, network, primDs);
        }
        else
#endif
        {
          graph = BuildShaderGraph(
              network, static_cast<HdCyclesSession *>(renderParam)->GetStageMetersPerUnit());
        }
        if (!graph) {
          TF_WARN("Could not build a shader graph for %s.", id.GetText());
        }
      }

      if (!graph) {
        /* Default shader if translation failed, so something is visible. */
        graph = make_unique<ShaderGraph>();
        PrincipledBsdfNode *bsdf = graph->create_node<PrincipledBsdfNode>();
        graph->connect(bsdf->output("BSDF"), graph->output()->input("Surface"));
      }

      AddInstanceIdAOV(graph.get());

      if (!_shader) {
        _shader = lock.scene->create_node<Shader>();
        // Cryptomatte material name
        _shader->name = id.GetString();
      }

      _shader->set_graph(std::move(graph));
      _shader->tag_update(lock.scene);
    }
  }

  *dirtyBits = DirtyBits::Clean;
}

/* An UsdTransform2d that does not transform, like the ones Nuke authors for every texture. */
static bool IsIdentityTransform2d(HdMaterialNodeSchema nodeSchema)
{
  HdMaterialNodeParameterContainerSchema params = nodeSchema.GetParameters();
  for (const TfToken &paramName : params.GetNames()) {
    auto valueDs = params.Get(paramName).GetValue();
    const VtValue value = valueDs ? valueDs->GetValue(0.0f) : VtValue();
    if ((paramName == "rotation" && value.IsHolding<float>() &&
         value.UncheckedGet<float>() != 0) ||
        (paramName == "scale" && value.IsHolding<GfVec2f>() &&
         value.UncheckedGet<GfVec2f>() != GfVec2f(1.0f)) ||
        (paramName == "translation" && value.IsHolding<GfVec2f>() &&
         value.UncheckedGet<GfVec2f>() != GfVec2f(0.0f)))
    {
      return false;
    }
  }
  return true;
}

/* Set the scale and bias of an UsdUVTexture on a multiply-add node after its color or alpha. */
static void SetTextureScaleBias(ShaderNode *node, const GfVec4f &scale, const GfVec4f &bias)
{
  if (node->type == VectorMathNode::get_node_type()) {
    VectorMathNode *math = static_cast<VectorMathNode *>(node);
    math->set_vector2(make_float3(scale[0], scale[1], scale[2]));
    math->set_vector3(make_float3(bias[0], bias[1], bias[2]));
  }
  else {
    MathNode *math = static_cast<MathNode *>(node);
    math->set_value2(scale[3]);
    math->set_value3(bias[3]);
  }
}

static const TfToken kNukeProjection("Project3DTexture");
static const TfToken kNukeCamera("NukeCameraOpInterface");

/* Parameter value of a material network node, the fallback when not authored. */
template<typename T>
static T GetNodeParameter(HdMaterialNodeSchema nodeSchema, const char *name, const T &fallback)
{
  HdMaterialNodeParameterContainerSchema params = nodeSchema.GetParameters();
  auto valueDs = params.Get(TfToken(name)).GetValue();
  const VtValue value = valueDs ? valueDs->GetValue(0.0f) : VtValue();
  return value.CanCast<T>() ? VtValue::Cast<T>(value).template UncheckedGet<T>() : fallback;
}

/* Name of the node connected to an input, empty when there is none. */
static TfToken GetUpstreamNodeName(HdMaterialNodeSchema nodeSchema, const char *input)
{
  HdMaterialConnectionVectorContainerSchema conns = nodeSchema.GetInputConnections();
  HdMaterialConnectionVectorSchema connVec = conns.Get(TfToken(input));
  if (connVec.GetNumElements() == 0) {
    return TfToken();
  }
  HdMaterialConnectionSchema connSchema = connVec.GetElement(0);
  return connSchema.GetUpstreamNodePath() ? connSchema.GetUpstreamNodePath()->GetTypedValue(0.0f) :
                                            TfToken();
}

/* Texture coordinates of Nuke's Project3DTexture, a projection from the camera of a
 * NukeCameraOpInterface node, computed like Nuke does: clip = P * inverse(xform) * projection *
 * format_matrix (row vectors, P in scene units) and uv = clip.xy / clip.w. Points the camera does
 * not project on (behind it, outside its clipping planes, on faces it does not project on) get
 * coordinates far outside the image, which a texture with CLIP extension makes transparent black.
 * shortcut: occlusion_mode (self or world shadowing of the projection) is not supported. */
struct NukeProjection {
  ShaderOutput *coordinates;
  /* 1 where the camera projects, else 0. */
  ShaderOutput *visible;
  /* Transparent outside the image, else its edge continues. */
  bool crop;
};

static NukeProjection BuildNukeProjection(ShaderGraph *graph,
                                          HdMaterialNodeSchema projection,
                                          HdMaterialNodeSchema camera,
                                          const double metersPerUnit)
{
  const GfMatrix4d xform = GetNodeParameter(camera, "xform", GfMatrix4d(1.0));
  const GfMatrix4d cameraProjection = GetNodeParameter(camera, "projection", GfMatrix4d(1.0));
  /* Nuke 17 renamed format_matrix. */
  const GfMatrix4d format = GetNodeParameter(
      projection, "format_matrix", GetNodeParameter(projection, "format_xform", GfMatrix4d(1.0)));
  /* Cycles world space (meters) to camera space, and to clip space. */
  const GfMatrix4d view = GfMatrix4d().SetScale(1.0 / metersPerUnit) * xform.GetInverse();
  const GfMatrix4d clip = view * cameraProjection * format;
  /* An orthographic projection keeps w at 1. */
  const bool orthographic = cameraProjection[2][3] == 0.0;
  const bool crop = GetNodeParameter(projection, "crop_to_format", 1) != 0;
  const int facesMode = GetNodeParameter(projection, "faces_mode", 0);

  auto math = [graph](NodeMathType type, ShaderOutput *a, ShaderOutput *b, const float bValue) {
    MathNode *node = graph->create_node<MathNode>();
    node->set_math_type(type);
    graph->connect(a, node->input("Value1"));
    if (b) {
      graph->connect(b, node->input("Value2"));
    }
    else {
      node->set_value2(bValue);
    }
    return node;
  };
  auto vector = [](const double x, const double y, const double z) {
    return make_float3(float(x), float(y), float(z));
  };

  GeometryNode *geometry = graph->create_node<GeometryNode>();
  ShaderOutput *position = geometry->output("Position");
  /* Column j of P * matrix. */
  auto component = [&](const GfMatrix4d &matrix, const int j) {
    VectorMathNode *dot = graph->create_node<VectorMathNode>();
    dot->set_math_type(NODE_VECTOR_MATH_DOT_PRODUCT);
    dot->set_vector2(vector(matrix[0][j], matrix[1][j], matrix[2][j]));
    graph->connect(position, dot->input("Vector1"));
    return math(NODE_MATH_ADD, dot->output("Value"), nullptr, float(matrix[3][j]))
        ->output("Value");
  };
  ShaderOutput *x = component(clip, 0);
  ShaderOutput *y = component(clip, 1);
  ShaderOutput *w = component(clip, 3);
  MathNode *u = math(NODE_MATH_DIVIDE, x, w, 0.0f);
  MathNode *v = math(NODE_MATH_DIVIDE, y, w, 0.0f);
  /* Without crop, the edge of the image continues outside it. */
  u->set_use_clamp(!crop);
  v->set_use_clamp(!crop);

  /* Depth in front of the camera, which looks down its -z axis. */
  ShaderOutput *depth =
      math(NODE_MATH_MULTIPLY, component(view, 2), nullptr, -1.0f)->output("Value");
  const bool nearClip = GetNodeParameter(projection, "near_clip_enable", 1) != 0;
  const bool farClip = GetNodeParameter(projection, "far_clip_enable", 1) != 0;
  ShaderOutput *visible = math(NODE_MATH_GREATER_THAN,
                               depth,
                               nullptr,
                               nearClip ? GetNodeParameter(camera, "near_plane", 0.0f) : 0.0f)
                              ->output("Value");
  if (farClip) {
    ShaderOutput *beforeFar =
        math(NODE_MATH_LESS_THAN, depth, nullptr, GetNodeParameter(camera, "far_plane", FLT_MAX))
            ->output("Value");
    visible = math(NODE_MATH_MULTIPLY, visible, beforeFar, 0.0f)->output("Value");
  }
  if (facesMode == 1 || facesMode == 2) {
    /* The outward normal (Cycles turns the true normal of back faces) towards the camera. */
    MathNode *side = math(NODE_MATH_MULTIPLY_ADD, geometry->output("Backfacing"), nullptr, -2.0f);
    side->set_value3(1.0f);
    VectorMathNode *normal = graph->create_node<VectorMathNode>();
    normal->set_math_type(NODE_VECTOR_MATH_SCALE);
    graph->connect(geometry->output("True Normal"), normal->input("Vector1"));
    graph->connect(side->output("Value"), normal->input("Scale"));
    VectorMathNode *facing = graph->create_node<VectorMathNode>();
    facing->set_math_type(NODE_VECTOR_MATH_DOT_PRODUCT);
    graph->connect(normal->output("Vector"), facing->input("Vector1"));
    if (orthographic) {
      /* The camera's +z axis. */
      facing->set_vector2(vector(xform[2][0], xform[2][1], xform[2][2]));
    }
    else {
      VectorMathNode *toCamera = graph->create_node<VectorMathNode>();
      toCamera->set_math_type(NODE_VECTOR_MATH_SUBTRACT);
      const GfVec3d cameraPosition = xform.ExtractTranslation() * metersPerUnit;
      toCamera->set_vector1(vector(cameraPosition[0], cameraPosition[1], cameraPosition[2]));
      graph->connect(position, toCamera->input("Vector2"));
      graph->connect(toCamera->output("Vector"), facing->input("Vector2"));
    }
    ShaderOutput *onFace = math(facesMode == 1 ? NODE_MATH_GREATER_THAN : NODE_MATH_LESS_THAN,
                                facing->output("Value"),
                                nullptr,
                                0.0f)
                               ->output("Value");
    visible = math(NODE_MATH_MULTIPLY, visible, onFace, 0.0f)->output("Value");
  }

  /* u + (1 - visible) * 1000 */
  MathNode *hidden = graph->create_node<MathNode>();
  hidden->set_math_type(NODE_MATH_SUBTRACT);
  hidden->set_value1(1.0f);
  graph->connect(visible, hidden->input("Value2"));
  MathNode *hiddenU = math(NODE_MATH_MULTIPLY_ADD, hidden->output("Value"), nullptr, 1000.0f);
  graph->connect(u->output("Value"), hiddenU->input("Value3"));

  CombineXYZNode *uv = graph->create_node<CombineXYZNode>();
  graph->connect(hiddenU->output("Value"), uv->input("X"));
  graph->connect(v->output("Value"), uv->input("Y"));
  return {uv->output("Vector"), visible, crop};
}

/* Nuke's surfaces and layer nodes, made of several Cycles nodes. Returns false for other nodes.
 * Their parameters are read here, the inputs connected later go to nodeDesc.inputs. */
bool HdCyclesMaterial::BuildNukeNode(ShaderGraph *graph,
                                     const TfToken &nodeType,
                                     HdMaterialNodeSchema nodeSchema,
                                     NodeDesc &nodeDesc)
{
  auto color = [](const GfVec3f &value) { return make_float3(value[0], value[1], value[2]); };
  /* An input times a constant factor, with the parameter value when not connected. */
  auto colorInput = [&](const char *name, const GfVec3f &fallback, const GfVec3f &factor) {
    VectorMathNode *node = graph->create_node<VectorMathNode>();
    node->set_math_type(NODE_VECTOR_MATH_MULTIPLY);
    node->set_vector1(color(GetNodeParameter(nodeSchema, name, fallback)));
    node->set_vector2(color(factor));
    nodeDesc.inputs[TfToken(name)] = node->input("Vector1");
    return node->output("Vector");
  };
  auto floatInput = [&](const char *name, const float fallback, const float factor) {
    MathNode *node = graph->create_node<MathNode>();
    node->set_math_type(NODE_MATH_MULTIPLY);
    node->set_value1(GetNodeParameter(nodeSchema, name, fallback));
    node->set_value2(factor);
    nodeDesc.inputs[TfToken(name)] = node->input("Value1");
    return node->output("Value");
  };
  auto setOutputs = [&](ShaderOutput *rgb, ShaderOutput *alpha) {
    for (const char *name : {"rgb", "rgba", "result"}) {
      nodeDesc.outputs[TfToken(name)] = rgb;
    }
    SeparateXYZNode *channels = graph->create_node<SeparateXYZNode>();
    graph->connect(rgb, channels->input("Vector"));
    nodeDesc.outputs[TfToken("r")] = channels->output("X");
    nodeDesc.outputs[TfToken("g")] = channels->output("Y");
    nodeDesc.outputs[TfToken("b")] = channels->output("Z");
    for (const char *name : {"a", "opacity"}) {
      nodeDesc.outputs[TfToken(name)] = alpha;
    }
  };
  /* A principled BSDF, with a mapping so the material output takes its BSDF. */
  auto surface = [&]() {
    PrincipledBsdfNode *bsdf = graph->create_node<PrincipledBsdfNode>();
    nodeDesc.node = bsdf;
    nodeDesc.mapping = sUsdToCyles->findUsd(CyclesMaterialTokens->UsdPreviewSurface);
    return bsdf;
  };

  if (nodeType == CyclesMaterialTokens->NukeDefaultSurface) {
    /* Nuke's surface of images and layers: lit diffuse only. BlendSurface scales its opacity.
     * shortcut: blend_op (how BlendSurface adds to what is behind) is not supported. */
    PrincipledBsdfNode *bsdf = surface();
    bsdf->set_specular_ior_level(0.0f);
    nodeDesc.inputs[TfToken("tex_color")] = bsdf->input("Base Color");
    graph->connect(
        floatInput("tex_opacity", 1.0f, GetNodeParameter(nodeSchema, "opacity_factor", 1.0f)),
        bsdf->input("Alpha"));
    return true;
  }
  if (nodeType == "BasicSurface") {
    /* Nuke's Phong surface: emission + diffuse + specular with a gain for each. */
    PrincipledBsdfNode *bsdf = surface();
    graph->connect(colorInput("diffuse_color",
                              GfVec3f(1.0f),
                              GetNodeParameter(nodeSchema, "diffuse_gain", GfVec3f(0.18f))),
                   bsdf->input("Base Color"));
    graph->connect(colorInput("emission_color",
                              GfVec3f(1.0f),
                              GetNodeParameter(nodeSchema, "emission_gain", GfVec3f(0.0f))),
                   bsdf->input("Emission Color"));
    bsdf->set_emission_strength(1.0f);
    graph->connect(colorInput("specular_color", GfVec3f(1.0f), GfVec3f(1.0f)),
                   bsdf->input("Specular Tint"));
    /* Nuke's default specular gain 0.8 as the default specular level. */
    const GfVec3f specularGain = GetNodeParameter(nodeSchema, "specular_gain", GfVec3f(0.8f));
    bsdf->set_specular_ior_level(0.5f * (specularGain[0] + specularGain[1] + specularGain[2]) /
                                 (3.0f * 0.8f));
    /* Nuke 17 has a roughness, Nuke 15 a Phong shininess: n -> GGX roughness (2 / (n + 2))^1/4.
     * shortcut: the shininess and roughness maps are not supported. */
    const float shininess = 0.5f * (GetNodeParameter(nodeSchema, "min_shininess", 10.0f) +
                                    GetNodeParameter(nodeSchema, "max_shininess", 10.0f));
    bsdf->set_roughness(GetNodeParameter(
        nodeSchema, "roughness", powf(2.0f / (std::max(shininess, 0.0f) + 2.0f), 0.25f)));
    graph->connect(
        floatInput("opacity_map", 1.0f, GetNodeParameter(nodeSchema, "opacity_factor", 1.0f)),
        bsdf->input("Alpha"));
    return true;
  }
  if (nodeType == "ConstantTexture" || nodeType == "FillShader") {
    /* A constant color (FillShader's has an alpha). */
    GfVec4f value(0.0f, 0.0f, 0.0f, 1.0f);
    if (nodeType == "FillShader") {
      value = GetNodeParameter(nodeSchema, "const_color", value);
    }
    else {
      const GfVec3f rgb = GetNodeParameter(nodeSchema, "const_color", GfVec3f(0.0f));
      value = GfVec4f(rgb[0], rgb[1], rgb[2], GetNodeParameter(nodeSchema, "const_opacity", 1.0f));
    }
    ColorNode *rgb = graph->create_node<ColorNode>();
    rgb->set_value(make_float3(value[0], value[1], value[2]));
    MathNode *alpha = graph->create_node<MathNode>();
    alpha->set_math_type(NODE_MATH_ADD);
    alpha->set_value1(value[3]);
    alpha->set_value2(0.0f);
    nodeDesc.node = rgb;
    setOutputs(rgb->output("Color"), alpha->output("Value"));
    return true;
  }
  if (nodeType == "MergeLayer") {
    /* Layer A on layer B. Operations: none, replace, over, stencil, mask, plus, max, min. */
    enum { NONE, REPLACE, OVER, STENCIL, MASK, PLUS, MAX, MIN };
    const int operation = GetNodeParameter(nodeSchema, "operation", int(OVER));
    ShaderOutput *a = colorInput("Argb", GfVec3f(0.0f), GfVec3f(1.0f));
    ShaderOutput *aAlpha = floatInput("Aopacity", 0.0f, 1.0f);
    ShaderOutput *b = colorInput("Brgb", GfVec3f(0.0f), GfVec3f(1.0f));
    ShaderOutput *bAlpha = floatInput("Bopacity", 0.0f, 1.0f);

    auto vectorMath = [&](NodeVectorMathType type, ShaderOutput *v1, ShaderOutput *v2) {
      VectorMathNode *node = graph->create_node<VectorMathNode>();
      node->set_math_type(type);
      graph->connect(v1, node->input("Vector1"));
      graph->connect(v2, node->input("Vector2"));
      return node;
    };
    auto math = [&](NodeMathType type, ShaderOutput *v1, ShaderOutput *v2) {
      MathNode *node = graph->create_node<MathNode>();
      node->set_math_type(type);
      graph->connect(v1, node->input("Value1"));
      graph->connect(v2, node->input("Value2"));
      return node;
    };
    MathNode *aInverse = graph->create_node<MathNode>();
    aInverse->set_math_type(NODE_MATH_SUBTRACT);
    aInverse->set_value1(1.0f);
    graph->connect(aAlpha, aInverse->input("Value2"));

    ShaderOutput *rgb = b;
    ShaderOutput *alpha = bAlpha;
    switch (operation) {
      case NONE:
        break;
      case REPLACE:
        rgb = a;
        alpha = aAlpha;
        break;
      case STENCIL:
      case MASK: {
        ShaderOutput *factor = operation == MASK ? aAlpha : aInverse->output("Value");
        rgb = vectorMath(NODE_VECTOR_MATH_MULTIPLY, b, factor)->output("Vector");
        alpha = math(NODE_MATH_MULTIPLY, bAlpha, factor)->output("Value");
        break;
      }
      case PLUS:
        rgb = vectorMath(NODE_VECTOR_MATH_ADD, a, b)->output("Vector");
        alpha = math(NODE_MATH_ADD, aAlpha, bAlpha)->output("Value");
        break;
      case MAX:
      case MIN:
        rgb = vectorMath(
                  operation == MAX ? NODE_VECTOR_MATH_MAXIMUM : NODE_VECTOR_MATH_MINIMUM, a, b)
                  ->output("Vector");
        alpha = math(operation == MAX ? NODE_MATH_MAXIMUM : NODE_MATH_MINIMUM, aAlpha, bAlpha)
                    ->output("Value");
        break;
      default: {
        /* Over: A + B * (1 - A alpha). */
        VectorMathNode *over = vectorMath(
            NODE_VECTOR_MATH_MULTIPLY_ADD, b, aInverse->output("Value"));
        graph->connect(a, over->input("Vector3"));
        rgb = over->output("Vector");
        MathNode *overAlpha = math(NODE_MATH_MULTIPLY_ADD, bAlpha, aInverse->output("Value"));
        graph->connect(aAlpha, overAlpha->input("Value3"));
        alpha = overAlpha->output("Value");
        break;
      }
    }
    nodeDesc.node = rgb->parent;
    setOutputs(rgb, alpha);
    return true;
  }
  return false;
}

/* Whether an input of a UsdPreviewSurface is ignored: specularColor is only used in the
 * specular workflow. */
static bool IsUnusedPreviewSurfaceInput(HdMaterialNodeParameterContainerSchema params,
                                        const TfToken &name)
{
  if (name != "specularColor") {
    return false;
  }
  auto valueDs = params.Get(TfToken("useSpecularWorkflow")).GetValue();
  const VtValue value = valueDs ? valueDs->GetValue(0.0f) : VtValue();
  return !(value.CanCast<int>() && VtValue::Cast<int>(value).UncheckedGet<int>() == 1);
}

void HdCyclesMaterial::UpdateParameters(NodeDesc &nodeDesc,
                                        HdMaterialNodeParameterContainerSchema params,
                                        const SdfPath &nodePath)
{
  for (const TfToken &paramName : params.GetNames()) {
    auto valueDs = params.Get(paramName).GetValue();
    if (!valueDs || IsUnusedPreviewSurfaceInput(params, paramName)) {
      continue;
    }
    VtValue value = valueDs->GetValue(0.0f);

    /* UsdUVTexture scale and bias multiply the texture, the image texture node has none. Its
     * "Scale" input would scale the texture coordinates. */
    if (nodeDesc.mapping && nodeDesc.node->type == ImageTextureNode::get_node_type() &&
        (paramName == "scale" || paramName == "bias"))
    {
      TextureScaleBias &scaleBias = _textureScaleBias[nodePath];
      if (value.CanCast<GfVec4f>()) {
        (paramName == "scale" ?
             scaleBias.scale :
             scaleBias.bias) = VtValue::Cast<GfVec4f>(value).UncheckedGet<GfVec4f>();
      }
      continue;
    }

    /* See if the parameter name is in USDPreviewSurface terms, and needs to be converted .*/
    const UsdToCyclesMapping *inputMapping = nodeDesc.mapping;
    const std::string inputName = inputMapping ?
                                      inputMapping->parameterName(paramName, nullptr, &value) :
                                      paramName.GetString();

    /* Find the input to write the parameter value to. */
    const SocketType *input = FindSocketType(nodeDesc.node->type, inputName);

    if (!input) {
      if (inputMapping) {
        if (inputMapping->hasParameterMapping(paramName)) {
          TF_WARN("USD input '%s' on node '%s' maps to unavailable Cycles input '%s' on '%s'",
                  paramName.GetText(),
                  nodePath.GetText(),
                  inputName.c_str(),
                  nodeDesc.node->name.c_str());
        }
        else {
          TF_WARN("Unsupported USD input '%s' on node '%s' ('%s'); value ignored",
                  paramName.GetText(),
                  nodePath.GetText(),
                  nodeDesc.node->name.c_str());
        }
      }
      else {
        TF_WARN("Could not find parameter '%s' on node '%s' ('%s')",
                paramName.GetText(),
                nodePath.GetText(),
                nodeDesc.node->name.c_str());
      }
      continue;
    }

    SetNodeValue(nodeDesc.node, *input, value);

    /* An empty color space (and UsdUVTexture's "auto") is automatic detection, not a color
     * space of that name. */
    if (input->type == SocketType::STRING && input->name == "colorspace") {
      const ustring colorspace = nodeDesc.node->get_string(*input);
      if (colorspace.empty() || colorspace == "auto") {
        nodeDesc.node->set(*input, u_colorspace_auto);
      }
      else if (colorspace == "raw") {
        nodeDesc.node->set(*input, u_colorspace_data);
      }
    }
  }
}

void HdCyclesMaterial::UpdateConnections(NodeDesc &nodeDesc,
                                         HdMaterialNodeSchema nodeSchema,
                                         const SdfPath &nodePath,
                                         ShaderGraph *shaderGraph)
{
  HdMaterialConnectionVectorContainerSchema conns = nodeSchema.GetInputConnections();
  for (const TfToken &dstSocketName : conns.GetNames()) {
    HdMaterialConnectionVectorSchema connVec = conns.Get(dstSocketName);
    const size_t count = connVec.GetNumElements();
    if (count == 0) {
      continue;
    }

    /* The shader passed through an AOV Output node is resolved at the terminals. */
    if (IsAovPassThrough(nodeDesc.node, dstSocketName) ||
        IsUnusedPreviewSurfaceInput(nodeSchema.GetParameters(), dstSocketName))
    {
      continue;
    }

    const UsdToCyclesMapping *inputMapping = nodeDesc.mapping;
    const std::string inputName = inputMapping ?
                                      inputMapping->parameterName(dstSocketName, nullptr) :
                                      dstSocketName.GetString();

    /* Find the input to connect to on the passed in node. */
    const auto inputIt = nodeDesc.inputs.find(dstSocketName);
    ShaderInput *input = inputIt != nodeDesc.inputs.end() ? inputIt->second :
                                                            FindInput(nodeDesc.node, inputName);

    if (!input) {
      TF_WARN("Ignoring connection on '%s.%s', input '%s' was not found",
              nodePath.GetText(),
              dstSocketName.GetText(),
              dstSocketName.GetText());
      continue;
    }

    /* USD allows N connections per input (MaterialX <switch>, <combine>, struct
     * inputs etc). Cycles inputs are single-connection, and the right lowering
     * depends on the node type, so just take the first and warn. */
    if (count > 1) {
      TF_WARN(
          "Ignoring multiple connections to '%s.%s'", nodePath.GetText(), dstSocketName.GetText());
    }

    HdMaterialConnectionSchema connSchema = connVec.GetElement(0);
    SdfPath upstreamNodePath = connSchema.GetUpstreamNodePath() ?
                                   MaterialNodeNameToSdfPath(
                                       connSchema.GetUpstreamNodePath()->GetTypedValue(0.0f)) :
                                   SdfPath();
    TfToken upstreamOutputName = connSchema.GetUpstreamNodeOutputName() ?
                                     connSchema.GetUpstreamNodeOutputName()->GetTypedValue(0.0f) :
                                     TfToken();

    /* Connect from the input of pass-through nodes, leaving out cycles. */
    size_t hops = 0;
    for (auto it = _passThrough.find(upstreamNodePath); it != _passThrough.end();
         it = _passThrough.find(upstreamNodePath))
    {
      std::tie(upstreamNodePath, upstreamOutputName) = it->second;
      if (upstreamNodePath.IsEmpty() || ++hops > _passThrough.size()) {
        upstreamNodePath = SdfPath();
        break;
      }
    }
    if (upstreamNodePath.IsEmpty()) {
      continue;
    }

    const auto srcNodeIt = _nodes.find(upstreamNodePath);
    if (srcNodeIt == _nodes.end()) {
      TF_WARN("Ignoring connection from '%s.%s' to '%s.%s', node '%s' was not found",
              upstreamNodePath.GetText(),
              upstreamOutputName.GetText(),
              nodePath.GetText(),
              dstSocketName.GetText(),
              upstreamNodePath.GetText());
      continue;
    }

    const UsdToCyclesMapping *outputMapping = srcNodeIt->second.mapping;
    const std::string outputName = outputMapping ?
                                       outputMapping->parameterName(upstreamOutputName, input) :
                                       upstreamOutputName.GetString();

    const auto outputIt = srcNodeIt->second.outputs.find(upstreamOutputName);
    ShaderOutput *output = outputIt != srcNodeIt->second.outputs.end() ?
                               outputIt->second :
                               FindOutput(srcNodeIt->second.node, outputName);

    if (!output) {
      TF_WARN("Ignoring connection from '%s.%s' to '%s.%s', output '%s' was not found",
              upstreamNodePath.GetText(),
              upstreamOutputName.GetText(),
              nodePath.GetText(),
              dstSocketName.GetText(),
              upstreamOutputName.GetText());
      continue;
    }

    const bool fromTexture = outputMapping &&
                             srcNodeIt->second.node->type == ImageTextureNode::get_node_type();
    const bool toNormal = nodeDesc.mapping &&
                          nodeDesc.node->type == PrincipledBsdfNode::get_node_type() &&
                          dstSocketName == CyclesMaterialTokens->normal;

    /* A normal map texture without scale and bias (as Nuke authors it) gives the color the
     * normal map node takes, the usual scale 2 and bias -1 map it to -1..1. */
    const bool normalMapColor = toNormal && fromTexture &&
                                _textureScaleBias[upstreamNodePath].scale == GfVec4f(1.0f) &&
                                _textureScaleBias[upstreamNodePath].bias == GfVec4f(0.0f);

    /* Scale and bias of an UsdUVTexture, after its color or alpha. */
    if (fromTexture && !normalMapColor) {
      const TextureScaleBias &scaleBias = _textureScaleBias[upstreamNodePath];
      ShaderNode *math;
      if (output->type() == SocketType::FLOAT) {
        MathNode *mathNode = shaderGraph->create_node<MathNode>();
        mathNode->set_math_type(NODE_MATH_MULTIPLY_ADD);
        shaderGraph->connect(output, mathNode->input("Value1"));
        output = mathNode->output("Value");
        math = mathNode;
      }
      else {
        VectorMathNode *mathNode = shaderGraph->create_node<VectorMathNode>();
        mathNode->set_math_type(NODE_VECTOR_MATH_MULTIPLY_ADD);
        shaderGraph->connect(output, mathNode->input("Vector1"));
        output = mathNode->output("Vector");
        math = mathNode;
      }
      SetTextureScaleBias(math, scaleBias.scale, scaleBias.bias);

      /* The r, g and b outputs are one channel of the color. */
      if (!toNormal && (upstreamOutputName == CyclesMaterialTokens->r ||
                        upstreamOutputName == CyclesMaterialTokens->g ||
                        upstreamOutputName == CyclesMaterialTokens->b))
      {
        SeparateXYZNode *separate = shaderGraph->create_node<SeparateXYZNode>();
        shaderGraph->connect(output, separate->input("Vector"));
        output = separate->output(upstreamOutputName == CyclesMaterialTokens->r ? "X" :
                                  upstreamOutputName == CyclesMaterialTokens->g ? "Y" :
                                                                                  "Z");
      }
    }

    /* The UsdPreviewSurface normal is in tangent space, the BSDF takes a world space normal. */
    if (toNormal) {
      if (!normalMapColor) {
        VectorMathNode *color = shaderGraph->create_node<VectorMathNode>();
        color->set_math_type(NODE_VECTOR_MATH_MULTIPLY_ADD);
        color->set_vector2(make_float3(0.5f, 0.5f, 0.5f));
        color->set_vector3(make_float3(0.5f, 0.5f, 0.5f));
        shaderGraph->connect(output, color->input("Vector1"));
        output = color->output("Vector");
      }
      NormalMapNode *normalMap = shaderGraph->create_node<NormalMapNode>();
      normalMap->set_space(NODE_NORMAL_MAP_TANGENT);
      shaderGraph->connect(output, normalMap->input("Color"));
      output = normalMap->output("Normal");
    }

    shaderGraph->connect(output, input);
  }
}

unique_ptr<ShaderGraph> HdCyclesMaterial::BuildShaderGraph(HdMaterialNetworkSchema network,
                                                           const double metersPerUnit)
{
  _nodes.clear();
  _passThrough.clear();
  _textureScaleBias.clear();

  unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();

  HdMaterialNodeContainerSchema nodes = network.GetNodes();

  /* Iterate all the nodes first and build a complete but unconnected graph with parameters set. */
  std::unordered_map<SdfPath, TfToken, SdfPath::Hash> nodeNames;
  /* Nodes built from other nodes, after these exist, and not connected like the others. */
  std::vector<TfToken> projections;
  std::unordered_set<SdfPath, SdfPath::Hash> notConnected;
  for (const TfToken &nodeName : nodes.GetNames()) {
    HdMaterialNodeSchema nodeSchema = nodes.Get(nodeName);
    const SdfPath nodePath = MaterialNodeNameToSdfPath(nodeName);
    nodeNames[nodePath] = nodeName;

    NodeDesc nodeDesc = {};

    const auto nodeIt = _nodes.find(nodePath);
    /* Create new node only if it does not exist yet. */
    if (nodeIt != _nodes.end()) {
      nodeDesc = nodeIt->second;
    }
    else {
      /* E.g. cycles_principled_bsdf or UsdPreviewSurface. */
      const TfToken nodeTypeIdToken = nodeSchema.GetNodeIdentifier() ?
                                          nodeSchema.GetNodeIdentifier()->GetTypedValue(0.0f) :
                                          TfToken();
      const std::string &nodeTypeId = nodeTypeIdToken.GetString();

      /* Leave out an UsdTransform2d that does not transform, connecting from its input. */
      if (nodeTypeIdToken == CyclesMaterialTokens->UsdTransform2d &&
          IsIdentityTransform2d(nodeSchema))
      {
        auto &source = _passThrough[nodePath];
        HdMaterialConnectionVectorContainerSchema conns = nodeSchema.GetInputConnections();
        HdMaterialConnectionVectorSchema connVec = conns.Get(TfToken("in"));
        if (connVec.GetNumElements() > 0) {
          HdMaterialConnectionSchema connSchema = connVec.GetElement(0);
          if (connSchema.GetUpstreamNodePath()) {
            source.first = MaterialNodeNameToSdfPath(
                connSchema.GetUpstreamNodePath()->GetTypedValue(0.0f));
          }
          if (connSchema.GetUpstreamNodeOutputName()) {
            source.second = connSchema.GetUpstreamNodeOutputName()->GetTypedValue(0.0f);
          }
        }
        continue;
      }

      /* Nuke's projection reads its camera from the camera node, which has no Cycles node. */
      if (nodeTypeIdToken == kNukeProjection || nodeTypeIdToken == kNukeCamera) {
        if (nodeTypeIdToken == kNukeProjection) {
          projections.push_back(nodeName);
        }
        notConnected.insert(nodePath);
        continue;
      }

      if (BuildNukeNode(graph.get(), nodeTypeIdToken, nodeSchema, nodeDesc)) {
        _nodes.emplace(nodePath, nodeDesc);
        continue;
      }

      ustring cyclesType(nodeTypeId);
      const bool isNativeNode = nodeTypeId.starts_with("cycles_") ||
                                nodeTypeId.starts_with("cycles:");
      if (isNativeNode) {
        /* Native Cycles note embedded in USDShade. */
        cyclesType = nodeTypeId.substr(strlen("cycles_"));
        nodeDesc.mapping = sUsdToCyles->findCycles(cyclesType);
      }
      else {
        /* Check if any remapping is needed (e.g. for USDPreviewSurface to Cycles nodes). */
        nodeDesc.mapping = sUsdToCyles->findUsd(nodeTypeIdToken);
        if (nodeDesc.mapping) {
          cyclesType = nodeDesc.mapping->nodeType();
        }
      }

      /* If it's a native Cycles' node-type, just do the lookup now. */
      if (const NodeType *nodeType = NodeType::find(cyclesType)) {
        nodeDesc.node = graph->create_node(nodeType);
        /* Image nodes start with scene linear, detect the color space unless one is set. */
        if (const SocketType *colorspace = nodeType->find_input(ustring("colorspace"))) {
          nodeDesc.node->set(*colorspace, u_colorspace_auto);
        }
        if (nodeDesc.mapping) {
          nodeDesc.mapping->applyDefaults(nodeDesc.node);
        }
        if (isNativeNode) {
          GetShaderNodeDefaults().Apply(nodeDesc.node, cyclesType.string());
        }
        _nodes.emplace(nodePath, nodeDesc);
      }
      else {
        TF_RUNTIME_ERROR("Could not create node '%s'", nodePath.GetText());
        continue;
      }
    }

    UpdateParameters(nodeDesc, nodeSchema.GetParameters(), nodePath);
  }

  /* A Nuke projection gives the outputs of the texture it projects, read with the coordinates
   * of the projection, from a texture node of its own: the image may also be projected by other
   * cameras, or used with its UVs. */
  for (const TfToken &nodeName : projections) {
    HdMaterialNodeSchema projection = nodes.Get(nodeName);
    const TfToken textureName = GetUpstreamNodeName(projection, "map_color");
    const TfToken cameraName = GetUpstreamNodeName(projection, "cam_xform");
    const SdfPath texturePath = MaterialNodeNameToSdfPath(textureName);
    const auto textureIt = _nodes.find(texturePath);
    if (textureIt == _nodes.end() || cameraName.IsEmpty() ||
        textureIt->second.node->type != ImageTextureNode::get_node_type())
    {
      TF_WARN("Projection '%s' has no texture or camera", nodeName.GetText());
      continue;
    }
    ImageTextureNode *texture = static_cast<ImageTextureNode *>(
        textureIt->second.node->clone(graph.get()));
    NodeDesc nodeDesc = {texture, textureIt->second.mapping};
    const NukeProjection result = BuildNukeProjection(
        graph.get(), projection, nodes.Get(cameraName), metersPerUnit);
    texture->set_extension(result.crop ? EXTENSION_CLIP : EXTENSION_EXTEND);
    graph->connect(result.coordinates, texture->input("Vector"));
    if (!result.crop) {
      /* The extended image edge has no transparent outside: hide the points the camera does not
       * project on. */
      VectorMathNode *color = graph->create_node<VectorMathNode>();
      color->set_math_type(NODE_VECTOR_MATH_MULTIPLY);
      graph->connect(texture->output("Color"), color->input("Vector1"));
      graph->connect(result.visible, color->input("Vector2"));
      MathNode *alpha = graph->create_node<MathNode>();
      alpha->set_math_type(NODE_MATH_MULTIPLY);
      graph->connect(texture->output("Alpha"), alpha->input("Value1"));
      graph->connect(result.visible, alpha->input("Value2"));
      for (const char *name : {"rgb", "rgba", "result", "r", "g", "b"}) {
        nodeDesc.outputs[TfToken(name)] = color->output("Vector");
      }
      for (const char *name : {"a", "opacity"}) {
        nodeDesc.outputs[TfToken(name)] = alpha->output("Value");
      }
    }
    const SdfPath projectionPath = MaterialNodeNameToSdfPath(nodeName);
    _textureScaleBias[projectionPath] = _textureScaleBias[texturePath];
    _nodes[projectionPath] = nodeDesc;
  }

  /* Now that all nodes have been constructed, iterate the network again and build up any
   * connections between nodes. */
  for (const TfToken &nodeName : nodes.GetNames()) {
    const SdfPath nodePath = MaterialNodeNameToSdfPath(nodeName);
    if (notConnected.count(nodePath)) {
      continue;
    }

    const auto nodeIt = _nodes.find(nodePath);
    if (nodeIt == _nodes.end()) {
      if (_passThrough.find(nodePath) == _passThrough.end()) {
        TF_RUNTIME_ERROR("Could not find node '%s' to connect", nodePath.GetText());
      }
      continue;
    }

    UpdateConnections(nodeIt->second, nodes.Get(nodeName), nodePath, graph.get());
  }

  /* Finally connect the terminals to the graph output (Surface, Volume, Displacement). */
  HdMaterialConnectionContainerSchema terminals = network.GetTerminals();
  for (const TfToken &terminalName : terminals.GetNames()) {
    HdMaterialConnectionSchema termSchema = terminals.Get(terminalName);
    SdfPath upstreamNodePath = termSchema.GetUpstreamNodePath() ?
                                   MaterialNodeNameToSdfPath(
                                       termSchema.GetUpstreamNodePath()->GetTypedValue(0.0f)) :
                                   SdfPath();
    TfToken upstreamOutputName = termSchema.GetUpstreamNodeOutputName() ?
                                     termSchema.GetUpstreamNodeOutputName()->GetTypedValue(0.0f) :
                                     TfToken();

    /* Houdini only exports nodes linked to the material output, so its AOV Output node passes the
     * shader connected to its "shader" input through to its output. */
    for (auto aovIt = _nodes.find(upstreamNodePath);
         aovIt != _nodes.end() && IsAovPassThrough(aovIt->second.node, upstreamOutputName);
         aovIt = _nodes.find(upstreamNodePath))
    {
      HdMaterialConnectionVectorSchema connVec =
          HdMaterialConnectionVectorContainerSchema(
              nodes.Get(nodeNames[upstreamNodePath]).GetInputConnections())
              .Get(kAovPassThroughInput);
      if (connVec.GetNumElements() == 0) {
        upstreamNodePath = SdfPath();
        break;
      }
      HdMaterialConnectionSchema connSchema = connVec.GetElement(0);
      upstreamNodePath = connSchema.GetUpstreamNodePath() ?
                             MaterialNodeNameToSdfPath(
                                 connSchema.GetUpstreamNodePath()->GetTypedValue(0.0f)) :
                             SdfPath();
      upstreamOutputName = connSchema.GetUpstreamNodeOutputName() ?
                               connSchema.GetUpstreamNodeOutputName()->GetTypedValue(0.0f) :
                               TfToken();
    }
    if (upstreamNodePath.IsEmpty()) {
      continue; /* Only an AOV Output node, nothing to shade with. */
    }

    const auto nodeIt = _nodes.find(upstreamNodePath);
    if (nodeIt == _nodes.end()) {
      TF_RUNTIME_ERROR("Could not find terminal node '%s'", upstreamNodePath.GetText());
      continue;
    }

    ShaderNode *const node = nodeIt->second.node;

    const char *inputName = nullptr;
    const char *outputName = nullptr;
    if (terminalName == HdMaterialTerminalTokens->surface ||
        terminalName == CyclesMaterialTokens->cyclesSurface)
    {
      inputName = "Surface";
      /* Find default output name based on the node if none is provided. */
      if (node->type->name == "add_closure" || node->type->name == "mix_closure") {
        outputName = "Closure";
      }
      else if (node->type->name == "emission") {
        outputName = "Emission";
      }
      else {
        outputName = "BSDF";
      }
    }
    else if (terminalName == HdMaterialTerminalTokens->displacement ||
             terminalName == CyclesMaterialTokens->cyclesDisplacement)
    {
      inputName = outputName = "Displacement";
    }
    else if (terminalName == HdMaterialTerminalTokens->volume ||
             terminalName == CyclesMaterialTokens->cyclesVolume)
    {
      inputName = outputName = "Volume";
    }
    else {
      /* Other terminals only pull nodes into the network, like an AOV Output node, which
       * writes its pass without being linked to the material output. */
      continue;
    }

    /* For native Cycles nodes we use the upstream output name as is, for
     * mapping from e.g. UsdPreviewSurface we need to use the default output
     * name that is known to exist. */
    if (!upstreamOutputName.IsEmpty() && nodeIt->second.mapping == nullptr) {
      outputName = upstreamOutputName.GetText();
    }

    /* Houdini's material builder authors every Cycles shader as surface terminal, also volume
     * shaders, which would leave volumes without a shader. */
    if (string(inputName) == "Surface" && string_iequals(outputName, "volume")) {
      inputName = "Volume";
    }

    ShaderInput *const input = inputName ? graph->output()->input(inputName) : nullptr;
    if (!input) {
      TF_RUNTIME_ERROR("Could not find terminal input '%s.%s'",
                       upstreamNodePath.GetText(),
                       inputName ? inputName : "<null>");
      continue;
    }

    ShaderOutput *const output = outputName ? FindOutput(node, outputName) : nullptr;
    if (!output) {
      TF_RUNTIME_ERROR("Could not find terminal output '%s.%s'",
                       upstreamNodePath.GetText(),
                       outputName ? outputName : "<null>");
      continue;
    }

    graph->connect(output, input);
  }

  /* Return no graph if nothing got successfully linked due unsupported nodes,
   * avoids the surface becoming transparennt then. */
  bool any_output_linked = false;
  for (ShaderInput *input : graph->output()->inputs) {
    any_output_linked |= input->link != nullptr;
  }
  if (!any_output_linked) {
    _nodes.clear();
    return nullptr;
  }

  return graph;
}

void HdCyclesMaterial::Finalize(HdRenderParam *renderParam)
{
  if (!_shader) {
    return;
  }

  const SceneLock lock(renderParam);
  const bool keep_nodes = static_cast<const HdCyclesSession *>(renderParam)->keep_nodes;

  _nodes.clear();

  if (!keep_nodes) {
    /* Cycles keeps deleted shaders, release the graph with its images. */
    _shader->set_graph(make_unique<ShaderGraph>());
    _shader->tag_update(lock.scene);
    lock.scene->delete_node(_shader);
  }
  _shader = nullptr;
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
