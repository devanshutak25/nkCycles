# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

"""
Dump the layout of Blender's Cycles shader nodes, to make the Houdini shader nodes look like them.

Run inside Blender:
    blender -b --factory-startup --python-exit-code 1 --python blender_node_layouts.py -- \
        <out.json> <blender source>/source/blender/nodes/shader/nodes

For every Blender shader node this writes the node label, the node properties (menus and options
drawn on the node) and input sockets in the order Blender draws them, the panels they are in,
their defaults, soft and hard ranges, for every input the menu values for which Blender hides
it, and the Add menu path of every node. The output is keyed by Blender node type; shader2hda.py maps it to the Cycles nodes.

The Python API only gives the generic range of a socket type (-10000 to 10000, or 0 to 1 for
factors), not the min and max a node declares for its socket (IOR 1 to 1000). Those are read from
the node declarations in the source of the same Blender version (the build hash is printed).
"""

import itertools
import json
import math
import os
import re
import sys

import bpy

# Nodes that are one Cycles node per value of a Blender menu: (Blender node, fixed properties).
VARIANTS = {
    "mix_float": ("ShaderNodeMix", {"data_type": "FLOAT"}),
    "mix_vector": ("ShaderNodeMix", {"data_type": "VECTOR", "factor_mode": "UNIFORM"}),
    "mix_vector_non_uniform": ("ShaderNodeMix", {"data_type": "VECTOR", "factor_mode": "NON_UNIFORM"}),
    "mix_color": ("ShaderNodeMix", {"data_type": "RGBA"}),
    "map_range": ("ShaderNodeMapRange", {"data_type": "FLOAT"}),
    "vector_map_range": ("ShaderNodeMapRange", {"data_type": "FLOAT_VECTOR"}),
}

# Panels of node declarations, which the Python API does not expose by name.
# (panel name, [properties and input names in the panel]), in declaration order.
PANELS = {
    "ShaderNodeBsdfPrincipled": [
        ("Diffuse", ["Diffuse Roughness"]),
        ("Subsurface", ["subsurface_method", "Subsurface Weight", "Subsurface Radius",
                        "Subsurface Scale", "Subsurface IOR", "Subsurface Anisotropy"]),
        ("Specular", ["distribution", "Specular IOR Level", "Specular Tint", "Anisotropic",
                      "Anisotropic Rotation", "Tangent"]),
        ("Transmission", ["Transmission Weight"]),
        ("Coat", ["Coat Weight", "Coat Roughness", "Coat IOR", "Coat Tint", "Coat Normal"]),
        ("Sheen", ["Sheen Weight", "Sheen Roughness", "Sheen Tint"]),
        ("Emission", ["Emission Color", "Emission Strength"]),
        ("Thin Film", ["Thin Film Thickness", "Thin Film IOR"]),
    ],
    "ShaderNodeBsdfGlass": [
        ("Thin Film", ["Thin Film Thickness", "Thin Film IOR"]),
    ],
    "ShaderNodeBsdfMetallic": [
        ("Thin Film", ["Thin Film Thickness", "Thin Film IOR"]),
    ],
    "ShaderNodeVolumeCoefficients": [
        ("Absorption", ["Absorption Coefficients"]),
        ("Scatter", ["phase", "Scatter Coefficients", "Anisotropy", "IOR", "Backscatter", "Alpha",
                     "Diameter"]),
        ("Emission", ["Emission Coefficients"]),
    ],
}

# Order in which draw_buttons() draws the properties, where it differs from the RNA order.
PROPERTY_ORDER = {
    "ShaderNodeTexImage": ["interpolation", "projection", "projection_blend", "extension"],
    "ShaderNodeTexEnvironment": ["interpolation", "projection"],
    "ShaderNodeTexBrick": ["offset", "offset_frequency", "squash", "squash_frequency"],
    "ShaderNodeTexVoronoi": ["voronoi_dimensions", "feature", "distance", "normalize"],
    "ShaderNodeMapRange": ["data_type", "interpolation_type", "clamp"],
}

# Properties that are not settings of the shader (output targets, internal state).
SKIP_PROPERTIES = {"is_active_output", "target", "active_index", "bytecode", "bytecode_hash",
                   "use_auto_update", "filepath", "mode"}

# Limit for the number of menu value combinations to test socket availability with.
MAX_COMBINATIONS = 4096


def to_json(value):
    if isinstance(value, (bool, int, float, str)) or value is None:
        return value
    try:
        return [float(x) for x in value]
    except (TypeError, ValueError):
        return str(value)


def node_properties(node):
    base = set(p.identifier for p in bpy.types.ShaderNode.bl_rna.properties)
    props = [p for p in node.bl_rna.properties
             if p.identifier not in base and p.identifier not in SKIP_PROPERTIES and
             not p.is_readonly and p.type in ("ENUM", "BOOLEAN", "INT", "FLOAT", "STRING")]
    order = PROPERTY_ORDER.get(node.bl_idname)
    if order:
        props.sort(key=lambda p: order.index(p.identifier) if p.identifier in order else len(order))
    return props


def property_info(node, prop):
    info = {
        "kind": "property",
        "identifier": prop.identifier,
        "label": prop.name,
        "description": prop.description,
        "type": prop.type,
    }
    if prop.type == "ENUM":
        if prop.is_enum_flag:
            return None
        valid = menu_values(node, prop)
        info["items"] = [[i.identifier, i.name, i.description] for i in prop.enum_items
                         if i.identifier in valid]
        info["default"] = getattr(node, prop.identifier)
    else:
        info["default"] = to_json(getattr(node, prop.identifier))
        if prop.type in ("INT", "FLOAT"):
            info["soft_range"] = [prop.soft_min, prop.soft_max]
            info["hard_range"] = [prop.hard_min, prop.hard_max]
    return info


def socket_info(node, socket, declared):
    info = {
        "kind": "input",
        "identifier": socket.identifier,
        "label": socket.name,
        "description": socket.description,
        "type": socket.type,
        "hide_value": socket.hide_value,
    }
    if hasattr(socket, "default_value"):
        info["default"] = to_json(socket.default_value)
        rna = socket.bl_rna.properties["default_value"]
        if hasattr(rna, "soft_min"):
            soft = [rna.soft_min, rna.soft_max]
            decl = declared.get(node.bl_idname, {}).get(socket.identifier)
            if decl:
                soft = [soft[0] if decl[0] is None else decl[0],
                        soft[1] if decl[1] is None else decl[1]]
            info["soft_range"] = soft
            info["hard_range"] = [rna.hard_min, rna.hard_max]
            info["subtype"] = rna.subtype
    return info


# Socket ranges from the node declarations.

DECLARATION_CONSTANTS = {"FLT_MAX": 3.4028234663852886e+38, "INT_MAX": 2147483647.0,
                         "M_PI": math.pi, "M_PI_2": math.pi / 2, "M_PI_4": math.pi / 4}


def closing(text, start):
    """Index of the bracket closing the one at start."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] in "({[":
            depth += 1
        elif text[i] in ")}]":
            depth -= 1
            if depth == 0:
                return i
    return len(text)


def statement_end(text, start):
    depth = 0
    for i in range(start, len(text)):
        if text[i] in "({[":
            depth += 1
        elif text[i] in ")}]":
            depth -= 1
        elif text[i] == ";" and depth == 0:
            return i
    return len(text)


def declared_number(statement, method):
    """Value of a .min(...) / .max(...) call in a socket declaration, or None."""
    match = re.search(r"\." + method + r"\(", statement)
    if not match:
        return None
    expr = statement[match.end():closing(statement, match.end() - 1)]
    expr = re.sub(r"(\d(?:\.\d*)?(?:e[-+]?\d+)?)f\b", r"\1", expr)
    expr = re.sub(r"\bfloat\(", "(", expr)
    try:
        return float(eval(expr, {"__builtins__": {}}, dict(DECLARATION_CONSTANTS)))
    except Exception:
        print("Warning: cannot evaluate .%s(%s)" % (method, expr))
        return None


def declared_ranges(source_dir):
    """{node idname: {socket identifier: [min, max]}} from the shader node declarations."""
    ranges = {}
    for filename in sorted(os.listdir(source_dir)):
        if not filename.endswith(".cc"):
            continue
        with open(os.path.join(source_dir, filename), encoding="utf-8") as f:
            text = f.read()
        declares = {}
        for match in re.finditer(r"static void (\w+)\(NodeDeclarationBuilder &b\)\s*\{", text):
            declares[match.group(1)] = text[match.end():closing(text, match.end() - 1)]
        for match in re.finditer(r'node_type_base\(&ntype,\s*"(\w+)"', text):
            declare = re.search(r"ntype\.declare\s*=\s*(?:\w+::)*(\w+);", text[match.end():])
            if not declare or declare.group(1) not in declares:
                continue
            body = declares[declare.group(1)]
            sockets = ranges.setdefault(match.group(1), {})
            for socket in re.finditer(r"add_input<decl::(?:Float|Int)>\(", body):
                statement = body[socket.start():statement_end(body, socket.start())]
                head_start = socket.end() - socket.start() - 1
                names = re.findall(r'"([^"]*)"', statement[head_start:closing(statement, head_start)])
                if not names:
                    continue
                low = declared_number(statement, "min")
                high = declared_number(statement, "max")
                if low is not None or high is not None:
                    sockets[names[-1]] = [low, high]
    return ranges


def availability(node, props, fixed):
    """For every input, the menu value combinations for which it is not available."""
    menus = [p for p in props if p.type == "ENUM" and not p.is_enum_flag and
             p.identifier not in fixed]
    values = [menu_values(node, p) for p in menus]
    combinations = list(itertools.product(*values))
    if len(combinations) > MAX_COMBINATIONS:
        raise RuntimeError("Too many menu combinations on %s" % node.bl_idname)

    hidden = {s.identifier: [] for s in node.inputs}
    available = {s.identifier: False for s in node.inputs}
    for combination in combinations:
        for prop, value in zip(menus, combination):
            setattr(node, prop.identifier, value)
        for socket in node.inputs:
            if socket.enabled:
                available[socket.identifier] = True
            else:
                hidden[socket.identifier].append(dict(zip((p.identifier for p in menus),
                                                          combination)))
    return hidden, available


def menu_values(node, prop):
    """Menu values that can be used in shader nodes (some items are for other node trees)."""
    current = getattr(node, prop.identifier)
    values = []
    for item in prop.enum_items:
        try:
            setattr(node, prop.identifier, item.identifier)
            values.append(item.identifier)
        except TypeError:
            pass
    setattr(node, prop.identifier, current)
    return values


def dump_node(tree, idname, fixed, declared):
    node = tree.nodes.new(idname)
    initial = {p.identifier: getattr(node, p.identifier) for p in node_properties(node)}
    for key, value in fixed.items():
        setattr(node, key, value)

    props = node_properties(node)
    hidden, available = availability(node, props, fixed)
    # Restore the initial values, availability testing changed them.
    for key, value in initial.items():
        if key not in fixed:
            setattr(node, key, value)

    items = []
    for prop in props:
        if prop.identifier in fixed:
            continue
        info = property_info(node, prop)
        if info:
            items.append(info)
    for socket in node.inputs:
        if not available[socket.identifier]:
            continue  # Never available, e.g. inputs only used by EEVEE.
        info = socket_info(node, socket, declared)
        reduced = reduce_conditions(hidden[socket.identifier], node, props, fixed)
        if reduced:
            info["hidden_when"] = reduced
        items.append(info)

    # Put panel items at the end, in panel order, and tag them with their panel.
    panels = PANELS.get(idname, [])
    panel_of = {}
    for panel, members in panels:
        for member in members:
            panel_of[member] = panel
    top = [i for i in items if (i["identifier"] if i["kind"] == "property" else i["label"])
           not in panel_of]
    in_panels = []
    for panel, members in panels:
        for member in members:
            for i in items:
                key = i["identifier"] if i["kind"] == "property" else i["label"]
                if key == member:
                    i["panel"] = panel
                    in_panels.append(i)
                    break
            else:
                if member not in ("Weight",):
                    raise RuntimeError("Panel item %s not found on %s" % (member, idname))

    outputs = [{"identifier": s.identifier, "label": s.name, "type": s.type}
               for s in node.outputs if s.enabled]
    result = {
        "blender_idname": idname,
        "label": node.bl_label,
        "fixed": fixed,
        "panels": [p for p, _ in panels],
        "items": top + in_panels,
        "outputs": outputs,
    }
    tree.nodes.remove(node)
    return result


def reduce_conditions(conditions, node, props, fixed):
    """Drop menus that do not affect the condition, and remove duplicates."""
    if not conditions:
        return []
    keys = list(conditions[0].keys())
    all_combinations = set()
    menus = {p.identifier: menu_values(node, p) for p in props
             if p.type == "ENUM" and not p.is_enum_flag and p.identifier not in fixed}
    hidden = set(tuple(c[k] for k in keys) for c in conditions)
    for key_index, key in enumerate(list(keys)):
        # A menu is irrelevant if for every hidden combination, all its values are hidden too.
        relevant = False
        for combination in hidden:
            for value in menus[key]:
                other = list(combination)
                other[key_index] = value
                if tuple(other) not in hidden:
                    relevant = True
                    break
            if relevant:
                break
        if not relevant:
            hidden = set(c[:key_index] + ("*",) + c[key_index + 1:] for c in hidden)
    for combination in hidden:
        all_combinations.add(tuple((k, v) for k, v in zip(keys, combination) if v != "*"))
    return [dict(c) for c in sorted(all_combinations)]


def menu_paths():
    """Blender node type -> Add menu path ("Utilities/Math"), parsed from the menu classes."""
    import bl_ui.node_add_menu_shader as menu_module
    with open(menu_module.__file__, encoding="utf-8") as f:
        text = f.read()
    menus = {}
    for block in re.split(r"\nclass ", text)[1:]:
        label = re.search(r'bl_label = "([^"]*)"', block)
        path = re.search(r'menu_path = "([^"]*)"', block)
        path = path.group(1) if path else (label.group(1) if label else "")
        if not path or path == "Root":
            continue
        for idname in re.findall(r'"(ShaderNode\w+)"', block):
            menus.setdefault(idname, path)
    return menus


def main():
    args = sys.argv[sys.argv.index("--") + 1:]
    if len(args) != 2:
        raise SystemExit("Usage: blender ... --python blender_node_layouts.py -- <out.json> "
                         "<blender source>/source/blender/nodes/shader/nodes")
    out_path, source_dir = args
    print("Blender %s, build hash %s: use the node source of this commit" %
          (bpy.app.version_string, bpy.app.build_hash.decode()))
    declared = declared_ranges(source_dir)
    bpy.context.scene.render.engine = "CYCLES"
    material = bpy.data.materials.new("layouts")
    material.use_nodes = True
    tree = material.node_tree

    nodes = {}
    for name in dir(bpy.types):
        cls = getattr(bpy.types, name)
        if not (isinstance(cls, type) and issubclass(cls, bpy.types.ShaderNode)):
            continue
        if cls is bpy.types.ShaderNode or name in ("ShaderNodeGroup", "ShaderNodeCustomGroup"):
            continue
        try:
            tree.nodes.remove(tree.nodes.new(name))
        except RuntimeError:
            continue  # Not available in shader node trees.
        nodes[name] = dump_node(tree, name, {}, declared)
    variants = {cycles: dump_node(tree, idname, fixed, declared)
                for cycles, (idname, fixed) in VARIANTS.items()}

    with open(out_path, "w", newline="\n") as f:
        json.dump({"blender_version": bpy.app.version_string, "nodes": nodes,
                   "variants": variants, "menus": menu_paths()}, f, indent=1, sort_keys=True)
        f.write("\n")
    print("Wrote %d nodes and %d variants to %s" % (len(nodes), len(variants), out_path))


main()
