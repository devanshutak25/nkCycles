#!/usr/bin/env hython
"""
Generate HDA files for all Cycles shader nodes from shader_nodes.json.

Usage:
    hython shader2hda.py --json <shader_nodes.json> --hda-install-dir <output_dir>

Each HDA is created via the shaderhda module (same pattern as osl2hda.py),
with parameter templates, menu enums, min/max ranges, and proper VOP types.
"""

import json
import os
import re
import sys
import traceback
import argparse

import hou
import shaderhda


def log(message, severity=hou.severityType.Message):
    print("[shader2hda] " + message)


def sanitize_parm_name(name):
    name = name.strip()
    name = re.sub(r"[^0-9a-zA-Z_]", "_", name)
    if re.match(r"^[0-9]", name):
        name = "_" + name
    if not name:
        name = "param"
    return name


def camel_to_spaced(name):
    name = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", name)
    name = re.sub(r"([A-Z])([A-Z][a-z])", r"\1 \2", name)
    name = name.replace("_", " ")
    words = []
    for w in name.split():
        if w.upper() in ("IOR", "BSDF", "SSS", "GGX", "OSL", "RGB", "UV", "AO"):
            words.append(w.upper())
        else:
            words.append(w.title())
    return " ".join(words)


VOP_TYPE_MAP = {
    "float": "float",
    "int": "int",
    "uint": "int",
    "uint64": "int",
    "boolean": "int",
    "menu": "int",
    "color": "color",
    "vector": "vector",
    "point": "vector",
    "normal": "vector",
    "point2": "vector",
    "closure": "surface",
    "string": "string",
    "matrix": "matrix",
    "node": "vector",
}


def vop_type(type_name):
    return VOP_TYPE_MAP.get(type_name, "float")


def is_vector_type(type_name):
    return type_name in ("color", "vector", "point", "normal", "point2", "node")


def create_parm_template(inp, shader_name=None):
    """Create a Houdini ParmTemplate from a JSON input definition."""
    name = inp["name"]
    label = inp.get("ui_name", camel_to_spaced(name))
    type_name = inp["type"]
    default = inp.get("default")
    enum_values = inp.get("enum_values")
    min_val = inp.get("min")
    max_val = inp.get("max")

    if type_name == "boolean":
        def_val = False
        if default is not None:
            def_val = bool(default)
        pt = hou.ToggleParmTemplate(name, label, default_value=def_val)
        return pt

    if type_name == "menu" and enum_values:
        items = []
        labels = []
        for token, disp in enum_values.items():
            items.append(token)
            labels.append(disp)
        menu_default = ""
        if default is not None:
            menu_default = str(default)
        pt = hou.StringParmTemplate(
            name, label, 1,
            default_value=(menu_default,),
            menu_items=items,
            menu_labels=labels,
            menu_type=hou.menuType.Normal,
        )
        return pt

    if type_name == "float":
        pt = hou.FloatParmTemplate(name, label, 1)
        if default is not None:
            pt.setDefaultValue((float(default),))
        if min_val is not None:
            pt.setMinValue(float(min_val))
            pt.setMinIsStrict(True)
        if max_val is not None:
            pt.setMaxValue(float(max_val))
            pt.setMaxIsStrict(True)
        return pt

    if type_name in ("int", "uint", "uint64"):
        pt = hou.IntParmTemplate(name, label, 1)
        if default is not None:
            pt.setDefaultValue((int(default),))
        return pt

    if type_name in ("color", "vector", "point", "normal"):
        num = 2 if type_name == "point2" else 3
        pt = hou.FloatParmTemplate(name, label, num)
        if default is not None and isinstance(default, (list, tuple)):
            vals = tuple(float(x) for x in default[:num])
            if len(vals) == num:
                pt.setDefaultValue(vals)
        if type_name == "color":
            pt.setLook(hou.parmLook.ColorSquare)
            pt.setNamingScheme(hou.parmNamingScheme.RGBA)
        return pt

    if type_name == "point2":
        pt = hou.FloatParmTemplate(name, label, 2)
        if default is not None and isinstance(default, (list, tuple)):
            vals = tuple(float(x) for x in default[:2])
            if len(vals) == 2:
                pt.setDefaultValue(vals)
        return pt

    if type_name == "matrix":
        pt = hou.FloatParmTemplate(name, label, 16)
        return pt

    if type_name == "string":
        pt = hou.StringParmTemplate(name, label, 1)
        if default is not None:
            pt.setDefaultValue((str(default),))
        return pt

    if type_name == "closure":
        return None

    if type_name == "node":
        pt = hou.FloatParmTemplate(name, label, 3)
        if default is not None and isinstance(default, (list, tuple)):
            vals = tuple(float(x) for x in default[:3])
            if len(vals) == 3:
                pt.setDefaultValue(vals)
        return pt

    pt = hou.FloatParmTemplate(name, label, 1)
    if default is not None:
        pt.setDefaultValue((float(default),))
    return pt


def create_shader_signature(node_data, shader_name):
    """Build a ShaderSignature from a JSON node definition."""
    node_name = node_data["name"]
    shader_label = "Cycles " + node_data.get("label", camel_to_spaced(node_name))
    inputs_def = node_data.get("inputs", [])
    outputs_def = node_data.get("outputs", [])

    used_input_names = set()
    shader_inputs = []
    for inp in inputs_def:
        name = inp["name"]
        base = name
        i = 1
        while name in used_input_names:
            name = base + "_" + str(i)
            i += 1
        used_input_names.add(name)

        vt = vop_type(inp["type"])
        ui_parm = create_parm_template(inp, node_name)

        shader_param = shaderhda.ShaderParameter(
            name,
            vt,
            ui_parm,
            None,
            True,
            shaderhda.ShaderParameterExtraInfos(),
        )
        shader_inputs.append(shader_param)

    used_output_names = set()
    shader_outputs = []
    for out in outputs_def:
        name = sanitize_parm_name(out["name"])
        base = name
        i = 1
        while name in used_output_names:
            name = base + "_" + str(i)
            i += 1
        used_output_names.add(name)

        vt = vop_type(out["type"])

        shader_param = shaderhda.ShaderParameter(
            name,
            vt,
            None,
            None,
            True,
            shaderhda.ShaderParameterExtraInfos(),
        )
        shader_outputs.append(shader_param)

    signature = shaderhda.ShaderSignature(
        shader_name,
        shader_label,
        shader_name,
        shader_inputs,
        shader_outputs,
    )
    return signature


def create_hda(node_data, install_dir):
    """Create an HDA for a single Cycles node."""
    node_name = node_data["name"]
    shader_name = "cycles_" + node_name
    shader_label = "Cycles " + node_data.get("label", camel_to_spaced(node_name))

    shader_hda = shaderhda.Shader(shader_name)

    if not shader_hda.shaderName():
        shader_hda.setShaderName(shader_name)

    if not shader_hda.hasHdaLabel():
        shader_hda.setHdaLabel(shader_label)

    if not shader_hda.vopnetMask():
        shader_hda.setVopnetMask("cycles")

    if not shader_hda.renderMask():
        shader_hda.setRenderMask("cycles")

    shader_hda.setShaderType("generic")
    shader_hda.setToolKeywords(["cycles"])
    shader_hda.setTabSubMenus(["Cycles"])

    signature = create_shader_signature(node_data, shader_name)
    shader_hda.signatureSet().addSignature(signature)
    shader_hda.signatureSet().sortSignatures()
    shader_hda.signatureSet().setBestDefaultSignature()

    hda_filename = (shader_name + ".hda").replace("cycles_", "cycles-")
    hda_path = os.path.join(install_dir, hda_filename)
    os.makedirs(install_dir, exist_ok=True)

    shader_hda.addHDAToFile(hda_path, False)

    hda_def = hou.hdaDefinition(
        hou.nodeTypeCategories()["Vop"], shader_name, hda_path
    )
    if hda_def:
        hda_def.setIcon("$CYCLES_ICON")
        hda_def.save(hda_path)

    log("Created: " + hda_path)
    return hda_path


def is_internal_node(name):
    """Nodes that Cycles creates itself and that are not useful to create by hand: type
    conversions inserted between sockets of different types, and the graph output."""
    return name.startswith("convert_") or name == "output"


def process_all(json_path, install_dir):
    with open(json_path, "r") as f:
        data = json.load(f)

    nodes = [n for n in data.get("nodes", []) if not is_internal_node(n.get("name", ""))]
    log("Loaded %d nodes from %s" % (len(nodes), json_path))

    successful = []
    failed = []

    for node_data in nodes:
        name = node_data.get("name", "<unknown>")
        try:
            create_hda(node_data, install_dir)
            successful.append(name)
        except Exception as e:
            log("Failed %s: %s" % (name, str(e)))
            traceback.print_exc()
            failed.append(name)

    log("=" * 60)
    log("PROCESSING SUMMARY")
    log("=" * 60)
    log("Total: %d" % len(nodes))
    log("Successful: %d" % len(successful))
    log("Failed: %d" % len(failed))
    if failed:
        for name in failed:
            log("  - " + name)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Generate Cycles shader HDAs from node registry JSON"
    )
    parser.add_argument("--json", required=True, help="Path to shader_nodes.json")
    parser.add_argument("--hda-install-dir", required=True, help="Output directory for .hda files")
    return parser.parse_args()


def main():
    args = parse_args()
    json_path = os.path.abspath(args.json)
    install_dir = os.path.abspath(args.hda_install_dir)

    log("Starting Cycles shader node to HDA conversion")
    log("JSON: %s" % json_path)
    log("Output: %s" % install_dir)

    if not os.path.exists(json_path):
        log("JSON file not found: %s" % json_path)
        return

    process_all(json_path, install_dir)
    log("Conversion complete")


if __name__ == "__main__":
    main()
