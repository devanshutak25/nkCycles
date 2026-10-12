/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/materialx.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"
#include "util/colorspace.h"

#include <pxr/base/tf/getenv.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/dataSourceMaterialNetworkInterface.h>
#include <pxr/imaging/hd/materialConnectionSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hdMtlx/hdMtlx.h>

#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>

#include <cctype>
#include <deque>
#include <map>
#include <set>
#include <tuple>

HDCYCLES_NAMESPACE_OPEN_SCOPE

namespace {

namespace mx = MaterialX;

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(MaterialXTokens,
    (mtlx)
    (kma)
    (surface)
    (displacement)
    (volume)
);
// clang-format on

/* What a MaterialX value is in the Cycles graph. Vector2 values are 3D vectors with Z = 0, the
 * fourth component of color4 and vector4 values is a separate float. */
enum class Kind { None, Float, Vec3, Vec4, Closure, String };

Kind KindOf(const std::string &type)
{
  if (type == "float" || type == "integer" || type == "boolean") {
    return Kind::Float;
  }
  if (type == "color3" || type == "vector3" || type == "vector2") {
    return Kind::Vec3;
  }
  if (type == "color4" || type == "vector4") {
    return Kind::Vec4;
  }
  if (type == "string" || type == "filename") {
    return Kind::String;
  }
  if (type == "BSDF" || type == "EDF" || type == "VDF" || type == "surfaceshader" ||
      type == "displacementshader" || type == "volumeshader" || type == "material")
  {
    return Kind::Closure;
  }
  return Kind::None;
}

/* A translated MaterialX value: a Cycles node output or a constant. */
struct Val {
  Kind kind = Kind::None;
  /* Float, Vec3 or Closure output, or the color of a Vec4. */
  ShaderOutput *out = nullptr;
  /* Fourth component of a Vec4. */
  ShaderOutput *alpha = nullptr;
  /* Constant, for the parts without an output. */
  float4 c = zero_float4();
  std::string str;
  /* Color space of a file name. */
  std::string colorspace;
  /* Value of a default geometric property (shading normal, tangent, ...), that Cycles nodes
   * already use when their input is not connected. */
  bool geomDefault = false;

  bool isConstant() const
  {
    return out == nullptr && alpha == nullptr;
  }
};

Val Const(const float x)
{
  Val v;
  v.kind = Kind::Float;
  v.c = make_float4(x, x, x, x);
  return v;
}

Val Const(const float x, const float y, const float z)
{
  Val v;
  v.kind = Kind::Vec3;
  v.c = make_float4(x, y, z, 1.0f);
  return v;
}

Val Of(ShaderOutput *out, const Kind kind)
{
  Val v;
  v.kind = kind;
  v.out = out;
  return v;
}

float Get(const float4 &c, const int i)
{
  return i == 0 ? c.x : i == 1 ? c.y : i == 2 ? c.z : c.w;
}

std::string NormalizeName(const std::string &name)
{
  std::string result;
  for (const char c : name) {
    if (c != '_' && c != ' ') {
      result += std::tolower(static_cast<unsigned char>(c));
    }
  }
  return result;
}

/* Node definition of a node. Inputs of other types than the definition (as Houdini authors when
 * e.g. a vector is connected to a color) need a rough match. */
mx::NodeDefPtr NodeDef(const mx::NodePtr &node)
{
  mx::NodeDefPtr nodeDef = node->getNodeDef();
  return nodeDef ? nodeDef : node->getNodeDef(mx::EMPTY_STRING, true);
}

bool IsSpace(const std::string &space, const char *name)
{
  return space == name || (std::string(name) == "object" && space == "model");
}

class Translator {
 public:
  Translator(ShaderGraph *graph, mx::DocumentPtr doc, const SdfPath &materialId)
      : _graph(graph), _doc(std::move(doc)), _materialId(materialId)
  {
  }

  /* Value of an output of a node in the document. */
  Val Eval(const mx::NodePtr &node, const std::string &output)
  {
    return Eval(node, output, nullptr);
  }

 private:
  /* Expansion of a node defined by a node graph: interface inputs of the graph resolve to the
   * inputs of the node, in the scope of the node. */
  struct Scope {
    mx::NodePtr instance;
    const Scope *parent;
  };

  ShaderGraph *_graph;
  mx::DocumentPtr _doc;
  SdfPath _materialId;
  std::deque<Scope> _scopes;
  std::map<std::tuple<const Scope *, const mx::Element *, std::string>, Val> _evaluated;
  std::map<ShaderOutput *, ShaderNode *> _separated;
  std::set<std::string> _warned;

  void Warn(const std::string &message)
  {
    if (_warned.insert(message).second) {
      TF_WARN("MaterialX material %s: %s", _materialId.GetText(), message.c_str());
    }
  }

  /* Cycles graph helpers. */

  ShaderNode *Create(const char *type)
  {
    const NodeType *nodeType = NodeType::find(ustring(type));
    assert(nodeType);
    return _graph->create_node(nodeType);
  }

  static ShaderInput *In(ShaderNode *node, const char *name)
  {
    const std::string normName = NormalizeName(name);
    for (ShaderInput *in : node->inputs) {
      if (NormalizeName(in->name().string()) == normName) {
        return in;
      }
    }
    for (ShaderInput *in : node->inputs) {
      if (NormalizeName(in->socket_type.name.string()) == normName) {
        return in;
      }
    }
    TF_CODING_ERROR("No input %s on Cycles node %s", name, node->type->name.c_str());
    return nullptr;
  }

  static ShaderOutput *Out(ShaderNode *node, const char *name)
  {
    const std::string normName = NormalizeName(name);
    for (ShaderOutput *out : node->outputs) {
      if (NormalizeName(out->name().string()) == normName ||
          NormalizeName(out->socket_type.name.string()) == normName)
      {
        return out;
      }
    }
    TF_CODING_ERROR("No output %s on Cycles node %s", name, node->type->name.c_str());
    return nullptr;
  }

  static void SetEnum(ShaderNode *node, const char *socketName, const char *value)
  {
    const SocketType *socket = node->type->find_input(ustring(socketName));
    if (socket && socket->enum_values && socket->enum_values->exists(ustring(value))) {
      node->set(*socket, ustring(value));
    }
    else {
      TF_CODING_ERROR("No value %s for %s on %s", value, socketName, node->type->name.c_str());
    }
  }

  static void SetString(ShaderNode *node, const char *socketName, const std::string &value)
  {
    if (const SocketType *socket = node->type->find_input(ustring(socketName))) {
      node->set(*socket, ustring(value));
    }
  }

  static void SetBool(ShaderNode *node, const char *socketName, const bool value)
  {
    if (const SocketType *socket = node->type->find_input(ustring(socketName))) {
      node->set(*socket, value);
    }
  }

  /* Connect a value to an input, or set it as constant. */
  void Link(const Val &v, ShaderInput *in)
  {
    if (!in || v.kind == Kind::None) {
      return;
    }
    if (v.out) {
      _graph->connect(v.out, in);
      return;
    }
    Node *node = in->parent;
    const SocketType &socket = in->socket_type;
    switch (socket.type) {
      case SocketType::FLOAT:
        node->set(socket, v.c.x);
        break;
      case SocketType::INT:
        node->set(socket, int(v.c.x));
        break;
      case SocketType::BOOLEAN:
        node->set(socket, v.c.x != 0.0f);
        break;
      case SocketType::COLOR:
      case SocketType::VECTOR:
      case SocketType::POINT:
      case SocketType::NORMAL:
        node->set(socket, make_float3(v.c.x, v.c.y, v.c.z));
        break;
      case SocketType::STRING:
        node->set(socket, ustring(v.str));
        break;
      default:
        break;
    }
  }

  /* Link unless the value is the default geometric property, that Cycles uses already. */
  void LinkGeom(const Val &v, ShaderInput *in)
  {
    if (!v.geomDefault) {
      Link(v, in);
    }
  }

  Val Component(const Val &v, const int i)
  {
    if (v.kind == Kind::Float || v.kind == Kind::None) {
      return v;
    }
    if (i == 3) {
      if (v.kind == Kind::Vec4 && v.alpha) {
        return Of(v.alpha, Kind::Float);
      }
      return Const(v.kind == Kind::Vec4 ? v.c.w : 1.0f);
    }
    if (!v.out) {
      return Const(Get(v.c, i));
    }
    ShaderNode *&separate = _separated[v.out];
    if (!separate) {
      separate = Create("separate_xyz");
      _graph->connect(v.out, In(separate, "Vector"));
    }
    return Of(Out(separate, i == 0 ? "X" : i == 1 ? "Y" : "Z"), Kind::Float);
  }

  Val Combine(const Val &x, const Val &y, const Val &z)
  {
    if (x.isConstant() && y.isConstant() && z.isConstant()) {
      return Const(x.c.x, y.c.x, z.c.x);
    }
    ShaderNode *node = Create("combine_xyz");
    Link(Cast(x, Kind::Float), In(node, "X"));
    Link(Cast(y, Kind::Float), In(node, "Y"));
    Link(Cast(z, Kind::Float), In(node, "Z"));
    return Of(Out(node, "Vector"), Kind::Vec3);
  }

  static Val WithAlpha(Val rgb, const Val &alpha)
  {
    rgb.kind = Kind::Vec4;
    rgb.alpha = alpha.out;
    rgb.c.w = alpha.c.x;
    return rgb;
  }

  Val Cast(Val v, const Kind kind)
  {
    if (kind == Kind::None || v.kind == kind || v.kind == Kind::None || v.kind == Kind::Closure ||
        v.kind == Kind::String)
    {
      return v;
    }
    switch (kind) {
      case Kind::Float:
        return Component(v, 0);
      case Kind::Vec3:
        if (v.kind == Kind::Float) {
          /* Cycles broadcasts a float output linked to a vector input. */
          v.c = make_float4(v.c.x, v.c.x, v.c.x, 1.0f);
        }
        v.kind = Kind::Vec3;
        v.alpha = nullptr;
        return v;
      case Kind::Vec4:
        if (v.kind == Kind::Float) {
          v.alpha = v.out;
          v.c = make_float4(v.c.x, v.c.x, v.c.x, v.c.x);
        }
        else {
          v.c.w = 1.0f;
          v.alpha = nullptr;
        }
        v.kind = Kind::Vec4;
        return v;
      default:
        return v;
    }
  }

  Val Math(const char *op, const Val &a, const Val &b = Const(0.0f), const Val &c = Const(0.0f))
  {
    ShaderNode *node = Create("math");
    SetEnum(node, "math_type", op);
    Link(Cast(a, Kind::Float), In(node, "Value1"));
    Link(Cast(b, Kind::Float), In(node, "Value2"));
    Link(Cast(c, Kind::Float), In(node, "Value3"));
    return Of(Out(node, "Value"), Kind::Float);
  }

  Val VMath(const char *op,
            const Val &a,
            const Val &b = Const(0.0f),
            const Val &c = Const(0.0f),
            const bool floatResult = false)
  {
    ShaderNode *node = Create("vector_math");
    SetEnum(node, "math_type", op);
    Link(Cast(a, Kind::Vec3), In(node, "Vector1"));
    Link(Cast(b, Kind::Vec3), In(node, "Vector2"));
    Link(Cast(c, Kind::Vec3), In(node, "Vector3"));
    return floatResult ? Of(Out(node, "Value"), Kind::Float) : Of(Out(node, "Vector"), Kind::Vec3);
  }

  Val Scale(const Val &v, const Val &scale)
  {
    ShaderNode *node = Create("vector_math");
    SetEnum(node, "math_type", "scale");
    Link(Cast(v, Kind::Vec3), In(node, "Vector1"));
    Link(Cast(scale, Kind::Float), In(node, "Scale"));
    return Of(Out(node, "Vector"), Kind::Vec3);
  }

  /* Component wise operation, with the vector math operation for 3D values if there is one. */
  Val Op(const Kind kind,
         const char *mathOp,
         const char *vectorOp,
         const Val &a,
         const Val &b = Const(0.0f),
         const Val &c = Const(0.0f))
  {
    switch (kind) {
      case Kind::Float:
        return Math(mathOp, a, b, c);
      case Kind::Vec3: {
        if (vectorOp) {
          return VMath(vectorOp, a, b, c);
        }
        const Val a3 = Cast(a, Kind::Vec3), b3 = Cast(b, Kind::Vec3), c3 = Cast(c, Kind::Vec3);
        Val result[3];
        for (int i = 0; i < 3; i++) {
          result[i] = Math(mathOp, Component(a3, i), Component(b3, i), Component(c3, i));
        }
        return Combine(result[0], result[1], result[2]);
      }
      case Kind::Vec4: {
        const Val a4 = Cast(a, Kind::Vec4), b4 = Cast(b, Kind::Vec4), c4 = Cast(c, Kind::Vec4);
        const Val rgb = Op(Kind::Vec3, mathOp, vectorOp, a4, b4, c4);
        return WithAlpha(rgb, Math(mathOp, Component(a4, 3), Component(b4, 3), Component(c4, 3)));
      }
      default:
        return Val();
    }
  }

  Val Mul(const Kind kind, const Val &a, const Val &b)
  {
    return Op(kind, "multiply", "multiply", a, b);
  }

  /* Mix from bg (factor 0) to fg (factor 1). */
  Val Mix(const Kind kind, const Val &fac, const Val &bg, const Val &fg)
  {
    switch (kind) {
      case Kind::Float: {
        ShaderNode *node = Create("mix_float");
        Link(Cast(fac, Kind::Float), In(node, "Factor"));
        Link(Cast(bg, Kind::Float), In(node, "A"));
        Link(Cast(fg, Kind::Float), In(node, "B"));
        return Of(Out(node, "Result"), Kind::Float);
      }
      case Kind::Vec3: {
        const bool uniform = fac.kind == Kind::Float;
        ShaderNode *node = Create(uniform ? "mix_vector" : "mix_vector_non_uniform");
        Link(uniform ? fac : Cast(fac, Kind::Vec3), In(node, "Factor"));
        Link(Cast(bg, Kind::Vec3), In(node, "A"));
        Link(Cast(fg, Kind::Vec3), In(node, "B"));
        return Of(Out(node, "Result"), Kind::Vec3);
      }
      case Kind::Vec4: {
        const Val bg4 = Cast(bg, Kind::Vec4), fg4 = Cast(fg, Kind::Vec4);
        const Val rgb = Mix(
            Kind::Vec3, fac.kind == Kind::Vec4 ? Cast(fac, Kind::Vec3) : fac, bg4, fg4);
        return WithAlpha(
            rgb, Mix(Kind::Float, Component(fac, 3), Component(bg4, 3), Component(fg4, 3)));
      }
      case Kind::Closure: {
        ShaderNode *node = Create("mix_closure");
        Link(Cast(fac, Kind::Float), In(node, "Fac"));
        Link(bg, In(node, "Closure1"));
        Link(fg, In(node, "Closure2"));
        return Of(Out(node, "Closure"), Kind::Closure);
      }
      default:
        return Val();
    }
  }

  Val AddClosures(const Val &a, const Val &b)
  {
    if (!a.out) {
      return b;
    }
    if (!b.out) {
      return a;
    }
    ShaderNode *node = Create("add_closure");
    Link(a, In(node, "Closure1"));
    Link(b, In(node, "Closure2"));
    return Of(Out(node, "Closure"), Kind::Closure);
  }

  /* Closure scaled by a weight, as Cycles closures only have weights through their colors. */
  Val Weighted(const Val &closure, const Val &weight)
  {
    if (weight.isConstant() && weight.c.x == 1.0f) {
      return closure;
    }
    ShaderNode *node = Create("mix_closure");
    Link(Cast(weight, Kind::Float), In(node, "Fac"));
    Link(closure, In(node, "Closure2"));
    return Of(Out(node, "Closure"), Kind::Closure);
  }

  Val Average(const Val &v)
  {
    if (v.kind != Kind::Vec3 && v.kind != Kind::Vec4) {
      return v;
    }
    if (v.isConstant()) {
      return Const((v.c.x + v.c.y + v.c.z) / 3.0f);
    }
    return VMath("dot_product", v, Const(1.0f / 3.0f), Const(0.0f), true);
  }

  /* MaterialX microfacet roughness is alpha (squared roughness), Cycles uses roughness. */
  Val RoughnessFromAlpha(const Val &alpha)
  {
    return Math("sqrt", Component(alpha, 0));
  }

  Val TexCoord()
  {
    ShaderNode *node = Create("texture_coordinate");
    return Of(Out(node, "UV"), Kind::Vec3);
  }

  Val Geometry(const char *output)
  {
    ShaderNode *node = Create("geometry");
    return Of(Out(node, output), Kind::Vec3);
  }

  Val Transform(const Val &v, const char *type, const char *from, const char *to)
  {
    ShaderNode *node = Create("vector_transform");
    SetEnum(node, "transform_type", type);
    SetEnum(node, "convert_from", from);
    SetEnum(node, "convert_to", to);
    Link(Cast(v, Kind::Vec3), In(node, "Vector"));
    return Of(Out(node, "Vector"), Kind::Vec3);
  }

  Val GeomProp(const mx::GeomPropDefPtr &geomProp, const Kind kind)
  {
    if (!geomProp) {
      return Val();
    }
    Val v = GeomProp(geomProp->getGeomProp(), geomProp->getSpace(), kind);
    v.geomDefault = true;
    return v;
  }

  Val GeomProp(const std::string &prop, const std::string &space, const Kind kind)
  {
    const bool object = IsSpace(space, "object");
    Val v;
    if (prop == "texcoord") {
      v = TexCoord();
    }
    else if (prop == "position") {
      v = object ? Of(Out(Create("texture_coordinate"), "Object"), Kind::Vec3) :
                   Geometry("Position");
    }
    else if (prop == "normal") {
      v = Geometry("Normal");
      if (object) {
        v = Transform(v, "normal", "world", "object");
      }
    }
    else if (prop == "tangent") {
      v = Geometry("Tangent");
      if (object) {
        v = Transform(v, "vector", "world", "object");
      }
    }
    else if (prop == "bitangent") {
      v = VMath("cross_product", Geometry("Normal"), Geometry("Tangent"));
    }
    else {
      ShaderNode *node = Create("attribute");
      SetString(node, "attribute", prop);
      v = Of(Out(node, "Vector"), Kind::Vec3);
    }
    return Cast(v, kind);
  }

  /* MaterialX graph helpers. */

  Val ValueOf(const mx::ValueElementPtr &element, const std::string &type)
  {
    Val v;
    v.kind = KindOf(type);
    if (v.kind == Kind::String) {
      v.str = type == "filename" ? element->getResolvedValueString() : element->getValueString();
      v.colorspace = element->getActiveColorSpace();
      return v;
    }
    const mx::ValuePtr value = element->getType() == type ? element->getValue() :
                                                            mx::Value::createValueFromStrings(
                                                                element->getValueString(), type);
    if (!value) {
      return v;
    }
    if (value->isA<float>()) {
      const float f = value->asA<float>();
      v.c = make_float4(f, f, f, f);
    }
    else if (value->isA<int>()) {
      const float f = float(value->asA<int>());
      v.c = make_float4(f, f, f, f);
    }
    else if (value->isA<bool>()) {
      const float f = value->asA<bool>() ? 1.0f : 0.0f;
      v.c = make_float4(f, f, f, f);
    }
    else if (value->isA<mx::Color3>()) {
      const mx::Color3 c = value->asA<mx::Color3>();
      v.c = make_float4(c[0], c[1], c[2], 1.0f);
    }
    else if (value->isA<mx::Color4>()) {
      const mx::Color4 c = value->asA<mx::Color4>();
      v.c = make_float4(c[0], c[1], c[2], c[3]);
    }
    else if (value->isA<mx::Vector2>()) {
      const mx::Vector2 c = value->asA<mx::Vector2>();
      v.c = make_float4(c[0], c[1], 0.0f, 1.0f);
    }
    else if (value->isA<mx::Vector3>()) {
      const mx::Vector3 c = value->asA<mx::Vector3>();
      v.c = make_float4(c[0], c[1], c[2], 1.0f);
    }
    else if (value->isA<mx::Vector4>()) {
      const mx::Vector4 c = value->asA<mx::Vector4>();
      v.c = make_float4(c[0], c[1], c[2], c[3]);
    }
    return v;
  }

  /* Value of a node input: its connection or value, or the default of the node definition. */
  Val Input(const mx::NodePtr &node, const std::string &name, const Scope *scope)
  {
    const mx::NodeDefPtr nodeDef = NodeDef(node);
    const mx::InputPtr input = node->getInput(name);
    const mx::InputPtr def = nodeDef ? nodeDef->getActiveInput(name) : nullptr;
    /* The type of the definition, inputs may be authored as another type (strings). */
    const std::string type = def ? def->getType() : input ? input->getType() : std::string();
    if (input) {
      const Val v = Resolve(input, scope, type);
      if (v.kind != Kind::None) {
        return Cast(v, KindOf(type));
      }
    }
    if (!def) {
      return Val();
    }
    if (def->hasDefaultGeomPropString()) {
      return GeomProp(def->getDefaultGeomProp(), KindOf(type));
    }
    return ValueOf(def, type);
  }

  /* Value an input or output refers to: an interface input, an output of a node or node graph,
   * or its own value. */
  Val Resolve(const mx::PortElementPtr &port, const Scope *scope, const std::string &type)
  {
    const mx::InputPtr input = port->asA<mx::Input>();
    if (input && input->hasInterfaceName() && scope) {
      return Input(scope->instance, input->getInterfaceName(), scope->parent);
    }
    if (port->hasNodeGraphString()) {
      const mx::NodeGraphPtr nodeGraph = _doc->getNodeGraph(port->getNodeGraphString());
      if (!nodeGraph) {
        Warn("missing node graph " + port->getNodeGraphString());
        return Val();
      }
      mx::OutputPtr output = port->hasOutputString() ?
                                 nodeGraph->getOutput(port->getOutputString()) :
                                 nullptr;
      if (!output && !nodeGraph->getOutputs().empty()) {
        output = nodeGraph->getOutputs().front();
      }
      return output ? Resolve(output, scope, type) : Val();
    }
    if (port->hasNodeName()) {
      const mx::ElementPtr parent = input ? port->getParent()->getParent() : port->getParent();
      const mx::GraphElementPtr graph = parent ? parent->asA<mx::GraphElement>() : nullptr;
      const mx::NodePtr upstream = graph ? graph->getNode(port->getNodeName()) : nullptr;
      if (!upstream) {
        Warn("missing node " + port->getNodeName());
        return Val();
      }
      return Eval(upstream, port->hasOutputString() ? port->getOutputString() : "out", scope);
    }
    if (input && (input->hasValueString() || KindOf(type) == Kind::String)) {
      return ValueOf(input, type);
    }
    return Val();
  }

  Val Eval(const mx::NodePtr &node, const std::string &output, const Scope *scope)
  {
    const auto key = std::make_tuple(scope, node.get(), output);
    const auto it = _evaluated.find(key);
    if (it != _evaluated.end()) {
      return it->second;
    }
    const Val v = EvalNode(node, output, scope);
    _evaluated[key] = v;
    return v;
  }

  Val EvalNode(const mx::NodePtr &node, const std::string &output, const Scope *scope)
  {
    const std::string &category = node->getCategory();
    const mx::NodeDefPtr nodeDef = NodeDef(node);
    if (!nodeDef) {
      Warn("no MaterialX node definition for node " + node->getName() + " (" + category + ")");
      return Val();
    }
    mx::OutputPtr defOutput = nodeDef->getOutput(output);
    if (!defOutput && !nodeDef->getOutputs().empty()) {
      defOutput = nodeDef->getOutputs().front();
    }
    const std::string outType = defOutput ? defOutput->getType() : node->getType();
    const Kind kind = KindOf(outType);

    const auto input = [&](const char *name) { return Input(node, name, scope); };

    /* Surface shaders map to the Principled BSDF, even when MaterialX defines them as node
     * graphs of its own BSDFs: Cycles has no layering of BSDFs. */
    if (category == "standard_surface") {
      return StandardSurface(input);
    }
    if (category == "open_pbr_surface") {
      return OpenPbrSurface(input);
    }
    if (category == "UsdPreviewSurface") {
      return UsdPreviewSurface(input);
    }
    if (category == "gltf_pbr") {
      return GltfPbr(input);
    }

    /* Nodes defined as node graphs are expanded. */
    const mx::InterfaceElementPtr implementation = nodeDef->getImplementation();
    if (implementation && implementation->isA<mx::NodeGraph>()) {
      const mx::NodeGraphPtr nodeGraph = implementation->asA<mx::NodeGraph>();
      mx::OutputPtr graphOutput = defOutput ? nodeGraph->getOutput(defOutput->getName()) : nullptr;
      if (!graphOutput && !nodeGraph->getOutputs().empty()) {
        graphOutput = nodeGraph->getOutputs().front();
      }
      if (!graphOutput) {
        Warn("node graph of " + nodeDef->getName() + " has no output " + output);
        return Val();
      }
      _scopes.push_back({node, scope});
      return Cast(Resolve(graphOutput, &_scopes.back(), outType), kind);
    }

    bool supported = true;
    Val v = Atomic(category, node, output, kind, outType, input, supported);
    if (!supported) {
      Warn("unsupported MaterialX node " + category + " (" + nodeDef->getName() + ")");
      Val zero;
      zero.kind = kind;
      return zero;
    }
    return Cast(v, kind);
  }

  template<typename InputFn>
  Val Atomic(const std::string &category,
             const mx::NodePtr &node,
             const std::string &output,
             const Kind kind,
             const std::string &outType,
             const InputFn &input,
             bool &supported)
  {
    /* Closures. */
    if (kind == Kind::Closure) {
      return Closure(category, input, supported);
    }

    /* Math. */
    static const std::map<std::string, std::pair<const char *, const char *>> unary = {
        {"absval", {"absolute", "absolute"}},
        {"floor", {"floor", "floor"}},
        {"ceil", {"ceil", "ceil"}},
        {"round", {"round", "round"}},
        {"sign", {"sign", "sign"}},
        {"sin", {"sine", "sine"}},
        {"cos", {"cosine", "cosine"}},
        {"tan", {"tangent", "tangent"}},
        {"asin", {"arcsine", nullptr}},
        {"acos", {"arccosine", nullptr}},
        {"sqrt", {"sqrt", nullptr}},
        {"fract", {"fraction", "fraction"}},
        {"exp", {"exponent", nullptr}},
    };
    static const std::map<std::string, std::pair<const char *, const char *>> binary = {
        {"add", {"add", "add"}},
        {"subtract", {"subtract", "subtract"}},
        {"multiply", {"multiply", "multiply"}},
        {"divide", {"divide", "divide"}},
        {"modulo", {"floored_modulo", nullptr}},
        {"power", {"power", "power"}},
        {"min", {"minimum", "minimum"}},
        {"max", {"maximum", "maximum"}},
    };
    if (const auto it = unary.find(category); it != unary.end()) {
      return Op(kind, it->second.first, it->second.second, input("in"));
    }
    if (const auto it = binary.find(category); it != binary.end()) {
      return Op(kind, it->second.first, it->second.second, input("in1"), input("in2"));
    }
    if (category == "ln") {
      return Op(kind, "logarithm", nullptr, input("in"), Const(2.718281828f));
    }
    if (category == "atan2") {
      const bool v139 = NodeDef(node)->getActiveInput("iny") != nullptr;
      return Op(
          kind, "arctan2", nullptr, input(v139 ? "iny" : "in1"), input(v139 ? "inx" : "in2"));
    }
    if (category == "clamp") {
      return Op(kind,
                "maximum",
                "maximum",
                Op(kind, "minimum", "minimum", input("in"), input("high")),
                input("low"));
    }
    if (category == "invert") {
      return Op(kind, "subtract", "subtract", input("amount"), input("in"));
    }
    if (category == "mix") {
      return Mix(kind, input("mix"), input("bg"), input("fg"));
    }
    if (category == "remap" || category == "smoothstep") {
      const bool smooth = category == "smoothstep";
      const Val in = input("in");
      const Val low = input(smooth ? "low" : "inlow");
      const Val high = input(smooth ? "high" : "inhigh");
      const Val outLow = smooth ? Const(0.0f) : input("outlow");
      const Val outHigh = smooth ? Const(1.0f) : input("outhigh");
      if (kind == Kind::Float) {
        return MapRange(in, low, high, outLow, outHigh, smooth);
      }
      const Val rgb = VectorMapRange(in, low, high, outLow, outHigh, smooth);
      if (kind == Kind::Vec4) {
        return WithAlpha(rgb,
                         MapRange(Component(in, 3),
                                  Component(low, 3),
                                  Component(high, 3),
                                  Component(outLow, 3),
                                  Component(outHigh, 3),
                                  smooth));
      }
      return rgb;
    }
    if (category == "dotproduct") {
      return VMath("dot_product", input("in1"), input("in2"), Const(0.0f), true);
    }
    if (category == "crossproduct") {
      return VMath("cross_product", input("in1"), input("in2"));
    }
    if (category == "normalize") {
      return VMath("normalize", input("in"));
    }
    if (category == "magnitude") {
      return VMath("length", input("in"), Const(0.0f), Const(0.0f), true);
    }
    if (category == "dot") {
      return input("in");
    }
    if (category == "constant") {
      return input("value");
    }
    if (category == "convert") {
      return Cast(input("in"), kind);
    }
    if (category == "extract") {
      return Component(input("in"), int(input("index").c.x));
    }
    if (category == "combine2" || category == "combine3" || category == "combine4") {
      const Val in1 = input("in1"), in2 = input("in2");
      if (category == "combine2") {
        if (kind == Kind::Vec4 && in1.kind == Kind::Vec3 && in2.kind == Kind::Vec3) {
          /* Vector4 from two vector2. */
          return WithAlpha(Combine(Component(in1, 0), Component(in1, 1), Component(in2, 0)),
                           Component(in2, 1));
        }
        if (kind == Kind::Vec4) {
          return WithAlpha(Cast(in1, Kind::Vec3), in2);
        }
        return Combine(in1, in2, Const(0.0f));
      }
      const Val rgb = Combine(in1, in2, input("in3"));
      return category == "combine4" ? WithAlpha(rgb, input("in4")) : rgb;
    }
    if (category == "luminance") {
      const Val in = input("in");
      const Val luminance = VMath("dot_product", in, input("lumacoeffs"), Const(0.0f), true);
      return kind == Kind::Vec4 ? WithAlpha(Cast(luminance, Kind::Vec3), Component(in, 3)) :
                                  luminance;
    }
    if (category == "rgbtohsv" || category == "hsvtorgb") {
      const Val in = input("in");
      Val rgb;
      if (category == "rgbtohsv") {
        ShaderNode *separate = Create("separate_color");
        SetEnum(separate, "color_type", "hsv");
        Link(Cast(in, Kind::Vec3), In(separate, "Color"));
        rgb = Combine(Of(Out(separate, "Red"), Kind::Float),
                      Of(Out(separate, "Green"), Kind::Float),
                      Of(Out(separate, "Blue"), Kind::Float));
      }
      else {
        ShaderNode *combine = Create("combine_color");
        SetEnum(combine, "color_type", "hsv");
        /* MaterialX wraps the hue, Cycles expects it in 0..1. */
        Link(Math("fraction", Component(in, 0)), In(combine, "Red"));
        Link(Component(in, 1), In(combine, "Green"));
        Link(Component(in, 2), In(combine, "Blue"));
        rgb = Of(Out(combine, "Color"), Kind::Vec3);
      }
      return kind == Kind::Vec4 ? WithAlpha(rgb, Component(in, 3)) : rgb;
    }
    if (category == "ifgreater" || category == "ifgreatereq" || category == "ifequal") {
      const Val value1 = input("value1"), value2 = input("value2");
      Val fac;
      if (category == "ifgreater") {
        fac = Math("greater_than", value1, value2);
      }
      else if (category == "ifgreatereq") {
        fac = Math("subtract", Const(1.0f), Math("less_than", value1, value2));
      }
      else {
        fac = Math("compare", value1, value2, Const(0.0f));
      }
      if (!NodeDef(node)->getActiveInput("in1")) {
        return fac; /* Boolean result. */
      }
      return Mix(kind, fac, input("in2"), input("in1"));
    }
    if (category == "and") {
      return Math("multiply", input("in1"), input("in2"));
    }
    if (category == "or") {
      return Math("maximum", input("in1"), input("in2"));
    }
    if (category == "not") {
      return Math("subtract", Const(1.0f), input("in"));
    }

    /* Geometry. */
    if (category == "texcoord") {
      return TexCoord();
    }
    if (category == "geomcolor" || category == "geompropvalue" ||
        category == "geompropvalueuniform")
    {
      ShaderNode *attribute = Create("attribute");
      SetString(attribute,
                "attribute",
                category == "geomcolor" ? std::string("displayColor") : input("geomprop").str);
      if (kind == Kind::Float) {
        return Of(Out(attribute, "Fac"), Kind::Float);
      }
      const Val rgb = Of(Out(attribute, outType.rfind("color", 0) == 0 ? "Color" : "Vector"),
                         Kind::Vec3);
      return kind == Kind::Vec4 ? WithAlpha(rgb, Of(Out(attribute, "Alpha"), Kind::Float)) : rgb;
    }
    if (category == "position" || category == "normal" || category == "tangent" ||
        category == "bitangent")
    {
      return GeomProp(category, input("space").str, kind);
    }
    if (category == "time" || category == "frame") {
      ShaderNode *time = Create("scene_time");
      return Of(Out(time, category == "time" ? "Seconds" : "Frame"), Kind::Float);
    }
    if (category == "viewdirection") {
      return Scale(Geometry("Incoming"), Const(-1.0f));
    }

    /* Textures. */
    if (category == "image" || category == "hextiledimage") {
      if (category == "hextiledimage") {
        Warn("hex tiled image is rendered as a plain image");
      }
      return Image(input, kind, outType);
    }
    if (category == "normalmap") {
      ShaderNode *normalMap = Create("normal_map");
      SetEnum(normalMap, "space", "tangent");
      Link(Component(input("scale"), 0), In(normalMap, "Strength"));
      Link(Cast(input("in"), Kind::Vec3), In(normalMap, "Color"));
      return Of(Out(normalMap, "Normal"), Kind::Vec3);
    }
    if (category == "heighttonormal") {
      ShaderNode *bump = Create("bump");
      Link(Cast(input("in"), Kind::Float), In(bump, "Height"));
      Link(Cast(input("scale"), Kind::Float), In(bump, "Distance"));
      return Of(Out(bump, "Normal"), Kind::Vec3);
    }
    if (category == "noise2d" || category == "noise3d" || category == "fractal3d" ||
        category == "fractal2d")
    {
      const bool is2d = category == "noise2d" || category == "fractal2d";
      ShaderNode *noise = Create("noise_texture");
      SetEnum(noise, "dimensions", is2d ? "2D" : "3D");
      SetEnum(noise, "type", "fBM");
      SetBool(noise, "use_normalize", false); /* Signed noise, like MaterialX. */
      Link(input(is2d ? "texcoord" : "position"), In(noise, "Vector"));
      Link(Const(1.0f), In(noise, "Scale"));
      if (category.rfind("fractal", 0) == 0) {
        Link(Math("subtract", input("octaves"), Const(1.0f)), In(noise, "Detail"));
        Link(input("lacunarity"), In(noise, "Lacunarity"));
        Link(input("diminish"), In(noise, "Roughness"));
      }
      else {
        Link(Const(0.0f), In(noise, "Detail"));
      }
      const Val noiseValue = kind == Kind::Float ? Of(Out(noise, "Fac"), Kind::Float) :
                                                   Of(Out(noise, "Color"), Kind::Vec3);
      Val v = Mul(kind == Kind::Float ? Kind::Float : Kind::Vec3, noiseValue, input("amplitude"));
      if (NodeDef(node)->getActiveInput("pivot")) {
        v = Op(v.kind, "add", "add", v, input("pivot"));
      }
      return v;
    }
    if (category == "cellnoise2d" || category == "cellnoise3d") {
      const bool is2d = category == "cellnoise2d";
      ShaderNode *noise = Create("white_noise_texture");
      SetEnum(noise, "dimensions", is2d ? "2D" : "3D");
      Link(VMath("floor", input(is2d ? "texcoord" : "position")), In(noise, "Vector"));
      return kind == Kind::Float ? Of(Out(noise, "Value"), Kind::Float) :
                                   Of(Out(noise, "Color"), Kind::Vec3);
    }
    if (category == "worleynoise2d" || category == "worleynoise3d") {
      const bool is2d = category == "worleynoise2d";
      ShaderNode *voronoi = Create("voronoi_texture");
      SetEnum(voronoi, "dimensions", is2d ? "2D" : "3D");
      SetEnum(voronoi, "feature", "f1");
      Link(input(is2d ? "texcoord" : "position"), In(voronoi, "Vector"));
      Link(Const(1.0f), In(voronoi, "Scale"));
      Link(input("jitter"), In(voronoi, "Randomness"));
      const bool solid = int(input("style").c.x) == 1;
      if (solid) {
        return Of(Out(voronoi, "Color"), Kind::Vec3);
      }
      return Of(Out(voronoi, "Distance"), Kind::Float);
    }
    if (category == "ramplr" || category == "ramptb" || category == "splitlr" ||
        category == "splittb")
    {
      const bool vertical = category == "ramptb" || category == "splittb";
      const Val coord = Component(input("texcoord"), vertical ? 1 : 0);
      Val fac;
      if (category.rfind("ramp", 0) == 0) {
        fac = Op(
            Kind::Float, "maximum", nullptr, Math("minimum", coord, Const(1.0f)), Const(0.0f));
      }
      else {
        fac = Math("greater_than", coord, input("center"));
      }
      return vertical ? Mix(kind, fac, input("valueb"), input("valuet")) :
                        Mix(kind, fac, input("valuel"), input("valuer"));
    }

    /* Compositing. */
    static const std::map<std::string, const char *> blends = {
        {"plus", "add"},
        {"minus", "subtract"},
        {"difference", "difference"},
        {"screen", "screen"},
        {"dodge", "dodge"},
        {"burn", "burn"},
    };
    if (const auto it = blends.find(category); it != blends.end()) {
      const Val fg = input("fg"), bg = input("bg"), mix = input("mix");
      const Val rgb = Blend(it->second, mix, bg, fg);
      if (kind == Kind::Float) {
        return Component(rgb, 0);
      }
      if (kind == Kind::Vec4) {
        return WithAlpha(rgb,
                         Component(Blend(it->second, mix, Component(bg, 3), Component(fg, 3)), 0));
      }
      return rgb;
    }
    if (category == "over") {
      const Val fg = input("fg"), bg = input("bg");
      const Val fgAlpha = Component(fg, 3);
      const Val invAlpha = Math("subtract", Const(1.0f), fgAlpha);
      const Val rgb = VMath("add", fg, Scale(bg, invAlpha));
      const Val alpha = Math("multiply_add", Component(bg, 3), invAlpha, fgAlpha);
      return Mix(kind, input("mix"), bg, WithAlpha(rgb, alpha));
    }
    if (category == "premult" || category == "unpremult") {
      const Val in = input("in");
      const Val alpha = Component(in, 3);
      const Val rgb = category == "premult" ? Scale(in, alpha) :
                                              VMath("divide", in, Cast(alpha, Kind::Vec3));
      return WithAlpha(rgb, alpha);
    }
    if (category == "inside" || category == "outside") {
      const Val mask = category == "inside" ? input("mask") :
                                              Math("subtract", Const(1.0f), input("mask"));
      return Mul(kind, input("in"), mask);
    }

    /* Transforms. */
    if (category == "rotate2d" || category == "rotate3d") {
      ShaderNode *rotate = Create("vector_rotate");
      SetEnum(rotate, "rotate_type", category == "rotate2d" ? "z_axis" : "axis");
      /* MaterialX rotates clockwise around the axis. */
      SetBool(rotate, "invert", true);
      Link(Cast(input("in"), Kind::Vec3), In(rotate, "Vector"));
      Link(Math("radians", input("amount")), In(rotate, "Angle"));
      if (category == "rotate3d") {
        Link(input("axis"), In(rotate, "Axis"));
      }
      return Of(Out(rotate, "Vector"), Kind::Vec3);
    }
    if (category == "transformpoint" || category == "transformvector" ||
        category == "transformnormal")
    {
      const char *type = category == "transformpoint"  ? "point" :
                         category == "transformvector" ? "vector" :
                                                         "normal";
      const char *from = IsSpace(input("fromspace").str, "object") ? "object" : "world";
      const char *to = IsSpace(input("tospace").str, "object") ? "object" : "world";
      if (std::string(from) == to) {
        return input("in");
      }
      return Transform(input("in"), type, from, to);
    }

    /* Physics. */
    if (category == "blackbody") {
      ShaderNode *blackbody = Create("blackbody");
      Link(input("temperature"), In(blackbody, "Temperature"));
      return Of(Out(blackbody, "Color"), Kind::Vec3);
    }
    if (category == "roughness_anisotropy") {
      const Val roughness = input("roughness");
      const Val alpha = Math("multiply", roughness, roughness);
      return Combine(alpha, alpha, Const(0.0f));
    }
    if (category == "roughness_dual") {
      const Val roughness = input("roughness");
      return Op(Kind::Vec3, "multiply", "multiply", roughness, roughness);
    }

    (void)output;
    supported = false;
    return Val();
  }

  Val MapRange(const Val &in,
               const Val &low,
               const Val &high,
               const Val &outLow,
               const Val &outHigh,
               const bool smooth)
  {
    ShaderNode *node = Create("map_range");
    SetEnum(node, "range_type", smooth ? "smoothstep" : "linear");
    SetBool(node, "clamp", smooth);
    Link(Cast(in, Kind::Float), In(node, "Value"));
    Link(Cast(low, Kind::Float), In(node, "From Min"));
    Link(Cast(high, Kind::Float), In(node, "From Max"));
    Link(Cast(outLow, Kind::Float), In(node, "To Min"));
    Link(Cast(outHigh, Kind::Float), In(node, "To Max"));
    return Of(Out(node, "Result"), Kind::Float);
  }

  Val VectorMapRange(const Val &in,
                     const Val &low,
                     const Val &high,
                     const Val &outLow,
                     const Val &outHigh,
                     const bool smooth)
  {
    ShaderNode *node = Create("vector_map_range");
    SetEnum(node, "range_type", smooth ? "smoothstep" : "linear");
    SetBool(node, "use_clamp", smooth);
    Link(Cast(in, Kind::Vec3), In(node, "Vector"));
    Link(Cast(low, Kind::Vec3), In(node, "From_Min_FLOAT3"));
    Link(Cast(high, Kind::Vec3), In(node, "From_Max_FLOAT3"));
    Link(Cast(outLow, Kind::Vec3), In(node, "To_Min_FLOAT3"));
    Link(Cast(outHigh, Kind::Vec3), In(node, "To_Max_FLOAT3"));
    return Of(Out(node, "Vector"), Kind::Vec3);
  }

  Val Blend(const char *type, const Val &fac, const Val &bg, const Val &fg)
  {
    ShaderNode *node = Create("mix_color");
    SetEnum(node, "blend_type", type);
    Link(Cast(fac, Kind::Float), In(node, "Factor"));
    Link(Cast(bg, Kind::Vec3), In(node, "A"));
    Link(Cast(fg, Kind::Vec3), In(node, "B"));
    return Of(Out(node, "Result"), Kind::Vec3);
  }

  static std::string CyclesColorSpace(const std::string &colorspace)
  {
    if (colorspace == "srgb_texture" || colorspace == "srgb_displayp3") {
      return u_colorspace_srgb.string();
    }
    if (colorspace == "lin_rec709" || colorspace == "lin_srgb") {
      return u_colorspace_scene_linear_srgb.string();
    }
    if (colorspace == "raw") {
      return u_colorspace_data.string();
    }
    if (colorspace == "acescg" || colorspace == "lin_ap1") {
      return "ACEScg";
    }
    /* Other names are looked up in the OpenColorIO configuration. */
    return colorspace;
  }

  template<typename InputFn>
  Val Image(const InputFn &input, const Kind kind, const std::string &outType)
  {
    const Val file = input("file");
    ShaderNode *image = Create("image_texture");
    SetString(image, "filename", file.str);
    /* MaterialX only color manages color images. Without a color space, detect it from the file
     * like Karma does (8 bit images are sRGB). */
    const bool isColor = outType.rfind("color", 0) == 0;
    const SocketType *colorspace = image->type->find_input(ustring("colorspace"));
    image->set(*colorspace,
               !isColor                ? u_colorspace_data :
               file.colorspace.empty() ? u_colorspace_auto :
                                         ustring(CyclesColorSpace(file.colorspace)));
    const std::string address = input("uaddressmode").str;
    SetEnum(image,
            "extension",
            address == "constant" ? "black" :
            address == "clamp"    ? "clamp" :
            address == "mirror"   ? "mirror" :
                                    "periodic");
    const std::string filter = input("filtertype").str;
    SetEnum(image,
            "interpolation",
            filter == "closest" ? "closest" :
            filter == "cubic"   ? "cubic" :
                                  "linear");
    Link(input("texcoord"), In(image, "Vector"));
    const Val color = Of(Out(image, "Color"), Kind::Vec3);
    switch (kind) {
      case Kind::Float:
        return Component(color, 0);
      case Kind::Vec4:
        return WithAlpha(color, Of(Out(image, "Alpha"), Kind::Float));
      default:
        return color;
    }
  }

  template<typename InputFn>
  Val Closure(const std::string &category, const InputFn &input, bool &supported)
  {
    if (category == "oren_nayar_diffuse_bsdf" || category == "burley_diffuse_bsdf") {
      ShaderNode *bsdf = Create("diffuse_bsdf");
      Link(Mul(Kind::Vec3, input("color"), input("weight")), In(bsdf, "Color"));
      Link(input("roughness"), In(bsdf, "Roughness"));
      LinkGeom(input("normal"), In(bsdf, "Normal"));
      return Of(Out(bsdf, "BSDF"), Kind::Closure);
    }
    if (category == "translucent_bsdf") {
      ShaderNode *bsdf = Create("translucent_bsdf");
      Link(Mul(Kind::Vec3, input("color"), input("weight")), In(bsdf, "Color"));
      LinkGeom(input("normal"), In(bsdf, "Normal"));
      return Of(Out(bsdf, "BSDF"), Kind::Closure);
    }
    if (category == "dielectric_bsdf") {
      const std::string mode = input("scatter_mode").str;
      if (mode == "R") {
        /* Fresnel weighted reflection: a Principled BSDF without diffuse. */
        ShaderNode *bsdf = Create("principled_bsdf");
        Link(Const(0.0f, 0.0f, 0.0f), In(bsdf, "Base Color"));
        Link(input("tint"), In(bsdf, "Specular Tint"));
        Link(input("ior"), In(bsdf, "IOR"));
        Link(RoughnessFromAlpha(input("roughness")), In(bsdf, "Roughness"));
        LinkGeom(input("normal"), In(bsdf, "Normal"));
        LinkGeom(input("tangent"), In(bsdf, "Tangent"));
        return Weighted(Of(Out(bsdf, "BSDF"), Kind::Closure), input("weight"));
      }
      ShaderNode *bsdf = Create("glass_bsdf");
      Link(Mul(Kind::Vec3, input("tint"), input("weight")), In(bsdf, "Color"));
      Link(input("ior"), In(bsdf, "IOR"));
      Link(RoughnessFromAlpha(input("roughness")), In(bsdf, "Roughness"));
      LinkGeom(input("normal"), In(bsdf, "Normal"));
      return Of(Out(bsdf, "BSDF"), Kind::Closure);
    }
    if (category == "conductor_bsdf" || category == "generalized_schlick_bsdf") {
      ShaderNode *bsdf = Create("metallic_bsdf");
      if (category == "conductor_bsdf") {
        SetEnum(bsdf, "fresnel_type", "physical_conductor");
        Link(input("ior"), In(bsdf, "IOR"));
        Link(input("extinction"), In(bsdf, "Extinction"));
      }
      else {
        SetEnum(bsdf, "fresnel_type", "f82");
        Link(input("color0"), In(bsdf, "Base Color"));
        Link(input("color90"), In(bsdf, "Edge Tint"));
      }
      Link(RoughnessFromAlpha(input("roughness")), In(bsdf, "Roughness"));
      LinkGeom(input("normal"), In(bsdf, "Normal"));
      LinkGeom(input("tangent"), In(bsdf, "Tangent"));
      return Weighted(Of(Out(bsdf, "BSDF"), Kind::Closure), input("weight"));
    }
    if (category == "sheen_bsdf") {
      ShaderNode *bsdf = Create("sheen_bsdf");
      SetEnum(bsdf, "distribution", "microfiber");
      Link(Mul(Kind::Vec3, input("color"), input("weight")), In(bsdf, "Color"));
      Link(input("roughness"), In(bsdf, "Roughness"));
      LinkGeom(input("normal"), In(bsdf, "Normal"));
      return Of(Out(bsdf, "BSDF"), Kind::Closure);
    }
    if (category == "subsurface_bsdf") {
      ShaderNode *bssrdf = Create("subsurface_scattering");
      SetEnum(bssrdf, "method", "random_walk");
      Link(Mul(Kind::Vec3, input("color"), input("weight")), In(bssrdf, "Color"));
      Link(input("radius"), In(bssrdf, "Radius"));
      Link(Const(1.0f), In(bssrdf, "Scale"));
      Link(input("anisotropy"), In(bssrdf, "Anisotropy"));
      LinkGeom(input("normal"), In(bssrdf, "Normal"));
      return Of(Out(bssrdf, "BSSRDF"), Kind::Closure);
    }
    if (category == "uniform_edf") {
      ShaderNode *emission = Create("emission");
      Link(input("color"), In(emission, "Color"));
      Link(Const(1.0f), In(emission, "Strength"));
      return Of(Out(emission, "Emission"), Kind::Closure);
    }
    if (category == "layer" || category == "add") {
      /* Cycles has no layering: the top layer is added, which is too bright where the top
       * reflects. */
      return AddClosures(input(category == "layer" ? "top" : "in1"),
                         input(category == "layer" ? "base" : "in2"));
    }
    if (category == "mix") {
      return Mix(Kind::Closure, input("mix"), input("bg"), input("fg"));
    }
    if (category == "multiply") {
      return Weighted(input("in1"), Average(input("in2")));
    }
    if (category == "surface") {
      return Opacity(AddClosures(input("bsdf"), input("edf")), input("opacity"));
    }
    if (category == "surface_unlit") {
      ShaderNode *emission = Create("emission");
      Link(input("emission_color"), In(emission, "Color"));
      Link(input("emission"), In(emission, "Strength"));
      return Opacity(Of(Out(emission, "Emission"), Kind::Closure), input("opacity"));
    }
    if (category == "displacement") {
      const Val displacement = input("displacement");
      if (displacement.isConstant() && displacement.c.x == 0.0f && displacement.c.y == 0.0f &&
          displacement.c.z == 0.0f)
      {
        return Val();
      }
      if (displacement.kind == Kind::Float) {
        ShaderNode *node = Create("displacement");
        SetEnum(node, "space", "object");
        Link(displacement, In(node, "Height"));
        Link(Const(0.0f), In(node, "Midlevel"));
        Link(input("scale"), In(node, "Scale"));
        return Of(Out(node, "Displacement"), Kind::Closure);
      }
      ShaderNode *node = Create("vector_displacement");
      SetEnum(node, "space", "object");
      Link(displacement, In(node, "Vector"));
      Link(Const(0.0f), In(node, "Midlevel"));
      Link(input("scale"), In(node, "Scale"));
      return Of(Out(node, "Displacement"), Kind::Closure);
    }
    supported = false;
    return Val();
  }

  Val Opacity(const Val &closure, const Val &opacity)
  {
    const Val fac = Average(opacity);
    if (fac.isConstant() && fac.c.x >= 1.0f) {
      return closure;
    }
    ShaderNode *transparent = Create("transparent_bsdf");
    Link(Const(1.0f, 1.0f, 1.0f), In(transparent, "Color"));
    return Mix(Kind::Closure, fac, Of(Out(transparent, "BSDF"), Kind::Closure), closure);
  }

  static bool IsZero(const Val &v)
  {
    return v.isConstant() && v.c.x == 0.0f;
  }

  /* Principled BSDF inputs set by the surface shader translations. */
  struct Principled {
    Val baseColor, metallic, roughness, ior, alpha, normal, diffuseRoughness;
    Val subsurfaceWeight, subsurfaceRadius, subsurfaceScale, subsurfaceAnisotropy;
    Val specularLevel, specularTint, anisotropic, anisotropicRotation, tangent;
    Val transmissionWeight;
    Val coatWeight, coatRoughness, coatIor, coatTint, coatNormal;
    Val sheenWeight, sheenRoughness, sheenTint;
    Val emissionColor, emissionStrength;
    Val thinFilmThickness, thinFilmIor, thinWall;
  };

  Val CreatePrincipled(const Principled &p)
  {
    ShaderNode *bsdf = Create("principled_bsdf");
    SetEnum(bsdf, "subsurface_method", "random_walk");
    const auto set = [&](const Val &v, const char *name) { Link(v, In(bsdf, name)); };
    set(p.baseColor, "Base Color");
    set(p.metallic, "Metallic");
    set(p.roughness, "Roughness");
    set(p.ior, "IOR");
    set(p.alpha, "Alpha");
    LinkGeom(p.normal, In(bsdf, "Normal"));
    set(p.diffuseRoughness, "Diffuse Roughness");
    set(p.subsurfaceWeight, "Subsurface Weight");
    set(p.subsurfaceRadius, "Subsurface Radius");
    set(p.subsurfaceScale, "Subsurface Scale");
    set(p.subsurfaceAnisotropy, "Subsurface Anisotropy");
    set(p.specularLevel, "Specular IOR Level");
    set(p.specularTint, "Specular Tint");
    set(p.anisotropic, "Anisotropic");
    set(p.anisotropicRotation, "Anisotropic Rotation");
    LinkGeom(p.tangent, In(bsdf, "Tangent"));
    set(p.transmissionWeight, "Transmission Weight");
    set(p.coatWeight, "Coat Weight");
    set(p.coatRoughness, "Coat Roughness");
    set(p.coatIor, "Coat IOR");
    set(p.coatTint, "Coat Tint");
    LinkGeom(p.coatNormal, In(bsdf, "Coat Normal"));
    set(p.sheenWeight, "Sheen Weight");
    set(p.sheenRoughness, "Sheen Roughness");
    set(p.sheenTint, "Sheen Tint");
    set(p.emissionColor, "Emission Color");
    set(p.emissionStrength, "Emission Strength");
    set(p.thinFilmThickness, "Thin Film Thickness");
    set(p.thinFilmIor, "Thin Film IOR");
    set(p.thinWall, "Thin Wall");
    return Of(Out(bsdf, "BSDF"), Kind::Closure);
  }

  /* The Principled BSDF uses the base color for subsurface scattering and transmission. */
  Val BaseColor(Val color,
                const Val &subsurfaceWeight,
                const Val &subsurfaceColor,
                const Val &transmissionWeight,
                const Val &transmissionColor)
  {
    if (!IsZero(subsurfaceWeight)) {
      color = Mix(Kind::Vec3, subsurfaceWeight, color, subsurfaceColor);
    }
    if (!IsZero(transmissionWeight)) {
      color = Mix(Kind::Vec3, transmissionWeight, color, transmissionColor);
    }
    return color;
  }

  template<typename InputFn> Val StandardSurface(const InputFn &input)
  {
    Principled p;
    p.baseColor = BaseColor(Mul(Kind::Vec3, input("base_color"), input("base")),
                            input("subsurface"),
                            input("subsurface_color"),
                            input("transmission"),
                            input("transmission_color"));
    p.metallic = input("metalness");
    p.diffuseRoughness = input("diffuse_roughness");
    p.roughness = input("specular_roughness");
    p.ior = input("specular_IOR");
    p.specularLevel = Math("multiply", input("specular"), Const(0.5f));
    p.specularTint = input("specular_color");
    p.anisotropic = input("specular_anisotropy");
    p.anisotropicRotation = input("specular_rotation");
    p.transmissionWeight = input("transmission");
    p.subsurfaceWeight = input("subsurface");
    p.subsurfaceRadius = input("subsurface_radius");
    p.subsurfaceScale = input("subsurface_scale");
    p.subsurfaceAnisotropy = input("subsurface_anisotropy");
    p.sheenWeight = input("sheen");
    p.sheenTint = input("sheen_color");
    p.sheenRoughness = input("sheen_roughness");
    p.coatWeight = input("coat");
    p.coatTint = input("coat_color");
    p.coatRoughness = input("coat_roughness");
    p.coatIor = input("coat_IOR");
    p.coatNormal = input("coat_normal");
    p.emissionStrength = input("emission");
    p.emissionColor = input("emission_color");
    p.thinFilmThickness = input("thin_film_thickness");
    p.thinFilmIor = input("thin_film_IOR");
    p.alpha = Average(input("opacity"));
    p.thinWall = input("thin_walled");
    p.normal = input("normal");
    p.tangent = input("tangent");
    return CreatePrincipled(p);
  }

  template<typename InputFn> Val OpenPbrSurface(const InputFn &input)
  {
    Principled p;
    const Val baseColor = input("base_color");
    p.baseColor = BaseColor(Mul(Kind::Vec3, baseColor, input("base_weight")),
                            input("subsurface_weight"),
                            input("subsurface_color"),
                            input("transmission_weight"),
                            input("transmission_color"));
    const Val coatWeight = input("coat_weight");
    if (!IsZero(coatWeight)) {
      /* Darkening of the base under the coat by internal reflections, as in the OpenPBR node
       * graph: (1 - Kcoat) / (1 - Ebase * Kcoat), which the Principled BSDF does not have. */
      const Val coatIor = input("coat_ior");
      const Val f0Sqrt = Math(
          "divide", Math("subtract", coatIor, Const(1.0f)), Math("add", coatIor, Const(1.0f)));
      const Val f0 = Math("multiply", f0Sqrt, f0Sqrt);
      const Val kCoat = Math(
          "subtract",
          Const(1.0f),
          Math("divide", Math("subtract", Const(1.0f), f0), Math("multiply", coatIor, coatIor)));
      const Val eDielectric = Mix(
          Kind::Vec3, input("subsurface_weight"), baseColor, input("subsurface_color"));
      const Val eBase = Mix(Kind::Vec3,
                            input("base_metalness"),
                            eDielectric,
                            Mul(Kind::Vec3, baseColor, input("specular_weight")));
      const Val darkening = VMath("divide",
                                  Cast(Math("subtract", Const(1.0f), kCoat), Kind::Vec3),
                                  VMath("subtract", Const(1.0f), Scale(eBase, kCoat)));
      const Val fac = Math("multiply", coatWeight, input("coat_darkening"));
      p.baseColor = Mul(Kind::Vec3, p.baseColor, Mix(Kind::Vec3, fac, Const(1.0f), darkening));
    }
    p.metallic = input("base_metalness");
    p.diffuseRoughness = input("base_diffuse_roughness");
    p.roughness = input("specular_roughness");
    p.ior = input("specular_ior");
    p.specularLevel = Math("multiply", input("specular_weight"), Const(0.5f));
    p.specularTint = input("specular_color");
    p.anisotropic = input("specular_roughness_anisotropy");
    p.transmissionWeight = input("transmission_weight");
    p.subsurfaceWeight = input("subsurface_weight");
    p.subsurfaceRadius = Mul(
        Kind::Vec3, input("subsurface_radius_scale"), input("subsurface_radius"));
    p.subsurfaceScale = Const(1.0f);
    p.subsurfaceAnisotropy = input("subsurface_scatter_anisotropy");
    p.sheenWeight = input("fuzz_weight");
    p.sheenTint = input("fuzz_color");
    p.sheenRoughness = input("fuzz_roughness");
    p.coatWeight = input("coat_weight");
    p.coatTint = input("coat_color");
    p.coatRoughness = input("coat_roughness");
    p.coatIor = input("coat_ior");
    p.coatNormal = input("geometry_coat_normal");
    p.emissionStrength = input("emission_luminance");
    p.emissionColor = input("emission_color");
    /* Micrometers in OpenPBR, nanometers in Cycles. */
    p.thinFilmThickness = Math("multiply",
                               Math("multiply", input("thin_film_thickness"), Const(1000.0f)),
                               input("thin_film_weight"));
    p.thinFilmIor = input("thin_film_ior");
    p.alpha = input("geometry_opacity");
    p.thinWall = input("geometry_thin_walled");
    p.normal = input("geometry_normal");
    p.tangent = input("geometry_tangent");
    return CreatePrincipled(p);
  }

  template<typename InputFn> Val UsdPreviewSurface(const InputFn &input)
  {
    Principled p;
    p.baseColor = input("diffuseColor");
    p.metallic = input("metallic");
    p.roughness = input("roughness");
    p.ior = input("ior");
    p.coatWeight = input("clearcoat");
    p.coatRoughness = input("clearcoatRoughness");
    p.emissionColor = input("emissiveColor");
    p.emissionStrength = Const(1.0f);
    p.alpha = input("opacity");
    p.normal = input("normal");
    return CreatePrincipled(p);
  }

  template<typename InputFn> Val GltfPbr(const InputFn &input)
  {
    Principled p;
    p.baseColor = input("base_color");
    p.metallic = input("metallic");
    p.roughness = input("roughness");
    p.ior = input("ior");
    p.transmissionWeight = input("transmission");
    p.specularLevel = Math("multiply", input("specular"), Const(0.5f));
    p.specularTint = input("specular_color");
    p.sheenWeight = Const(1.0f);
    p.sheenTint = input("sheen_color");
    p.sheenRoughness = input("sheen_roughness");
    p.coatWeight = input("clearcoat");
    p.coatRoughness = input("clearcoat_roughness");
    p.coatNormal = input("clearcoat_normal");
    p.emissionColor = input("emissive");
    p.emissionStrength = input("emissive_strength");
    /* Alpha mode 0 is opaque. */
    p.alpha = IsZero(input("alpha_mode")) ? Const(1.0f) : input("alpha");
    p.normal = input("normal");
    p.tangent = input("tangent");
    return CreatePrincipled(p);
  }
};

/* The node of the document that the terminal connects to. */
mx::NodePtr FindTerminalNode(const mx::DocumentPtr &doc, const std::string &name)
{
  if (mx::NodePtr node = doc->getNode(name)) {
    return node;
  }
  for (const mx::NodePtr &material : doc->getMaterialNodes()) {
    for (const mx::InputPtr &input : material->getInputs()) {
      if (mx::NodePtr node = input->getConnectedNode()) {
        return node;
      }
    }
  }
  return nullptr;
}

}  // namespace

const TfTokenVector &MaterialXRenderContexts()
{
  static const TfTokenVector contexts = {MaterialXTokens->mtlx, MaterialXTokens->kma};
  return contexts;
}

bool IsMaterialXNetwork(const HdMaterialNetworkSchema &network)
{
  HdMaterialNodeContainerSchema nodes = network.GetNodes();
  HdMaterialConnectionContainerSchema terminals = network.GetTerminals();
  for (const TfToken &terminalName : terminals.GetNames()) {
    HdMaterialConnectionSchema terminal = terminals.Get(terminalName);
    if (!terminal.GetUpstreamNodePath()) {
      continue;
    }
    HdMaterialNodeSchema node = nodes.Get(terminal.GetUpstreamNodePath()->GetTypedValue(0.0f));
    if (node && node.GetNodeIdentifier() &&
        TfStringStartsWith(node.GetNodeIdentifier()->GetTypedValue(0.0f).GetString(), "ND_"))
    {
      return true;
    }
  }
  return false;
}

unique_ptr<ShaderGraph> BuildMaterialXShaderGraph(const SdfPath &materialId,
                                                  const HdMaterialNetworkSchema &network,
                                                  const HdContainerDataSourceHandle &primContainer)
{
  const mx::DocumentPtr &libraries = HdMtlxStdLibraries();
  if (!libraries) {
    TF_WARN("MaterialX libraries not found, can not translate %s", materialId.GetText());
    return nullptr;
  }

  HdDataSourceMaterialNetworkInterface networkInterface(
      materialId, network.GetContainer(), primContainer);

  unique_ptr<ShaderGraph> graph = make_unique<ShaderGraph>();
  bool anyLinked = false;
  static const bool debug = TfGetenvBool("HD_CYCLES_MTLX_DEBUG", false);

  for (const TfToken &terminalName : networkInterface.GetTerminalNames()) {
    const char *graphInput = terminalName == MaterialXTokens->surface      ? "Surface" :
                             terminalName == MaterialXTokens->displacement ? "Displacement" :
                             terminalName == MaterialXTokens->volume       ? "Volume" :
                                                                             nullptr;
    if (!graphInput) {
      continue;
    }
    const HdMaterialNetworkInterface::InputConnectionResult connection =
        networkInterface.GetTerminalConnection(terminalName);
    if (!connection.first) {
      continue;
    }
    const TfToken &terminalNode = connection.second.upstreamNodeName;
    const TfTokenVector connectionNames = networkInterface.GetNodeInputConnectionNames(
        terminalNode);

    mx::DocumentPtr doc;
    try {
      doc = HdMtlxCreateMtlxDocumentFromHdMaterialNetworkInterface(
          &networkInterface, terminalNode, connectionNames, libraries);
    }
    catch (const mx::Exception &e) {
      TF_WARN("Could not create a MaterialX document for %s: %s", materialId.GetText(), e.what());
      continue;
    }
    if (!doc) {
      continue;
    }
    if (debug) {
      mx::XmlWriteOptions options;
      options.writeXIncludeEnable = false;
      options.elementPredicate = [](const mx::ConstElementPtr &element) {
        return !element->hasSourceUri();
      };
      fprintf(stderr,
              "[hdCycles] MaterialX document for %s %s:\n%s\n",
              materialId.GetText(),
              terminalName.GetText(),
              mx::writeToXmlString(doc, &options).c_str());
    }

    const mx::NodePtr node = FindTerminalNode(doc,
                                              HdMtlxCreateNameFromPath(SdfPath(terminalNode)));
    if (!node) {
      TF_WARN(
          "No MaterialX terminal node for %s %s", materialId.GetText(), terminalName.GetText());
      continue;
    }

    Translator translator(graph.get(), doc, materialId);
    const Val result = translator.Eval(node, "out");
    if (result.out) {
      graph->connect(result.out, graph->output()->input(graphInput));
      anyLinked = true;
    }
  }

  if (!anyLinked) {
    return nullptr;
  }
  return graph;
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
