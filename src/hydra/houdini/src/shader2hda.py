#!/usr/bin/env hython
"""
Generate HDA files for all Cycles shader nodes from shader_nodes.json.

Usage:
    hython shader2hda.py --json <shader_nodes.json> --hda-install-dir <output_dir>
        [--layouts <blender_node_layouts.json>] [--defaults-out <shader_defaults.json>]

Each HDA is created via the shaderhda module (same pattern as osl2hda.py),
with parameter templates, menu enums, min/max ranges, and proper VOP types.

With --layouts, the nodes are laid out like Blender's shader nodes: Blender's labels, parameter
order, menus, defaults, ranges and panels, and inputs are hidden when Blender hides them. Houdini
only writes parameters that differ from their default to USD, so the defaults that differ from
the Cycles socket defaults are written to --defaults-out, for the render delegate to apply.
"""

import json
import os
import re
import sys
import traceback
import argparse

import hou
import shaderhda

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blender_layout  # noqa: E402


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


def set_range(pt, param):
    if not param["range"]:
        return
    low, high, low_strict, high_strict = param["range"]
    pt.setMinValue(low)
    pt.setMaxValue(high)
    pt.setMinIsStrict(low_strict)
    pt.setMaxIsStrict(high_strict)


def create_parm_template(name, param):
    """Create a Houdini ParmTemplate for a laid out Cycles socket."""
    socket = param["socket"]
    label = param["label"]
    type_name = socket["type"]
    default = param["default"]

    if type_name == "boolean":
        return hou.ToggleParmTemplate(name, label, default_value=bool(default))

    if type_name == "menu" and param["menu"]:
        items = [token for token, _ in param["menu"]]
        labels = [item_label for _, item_label in param["menu"]]
        return hou.StringParmTemplate(
            name, label, 1,
            default_value=(str(default) if default is not None else "",),
            menu_items=items,
            menu_labels=labels,
            menu_type=hou.menuType.Normal,
        )

    if type_name == "float":
        pt = hou.FloatParmTemplate(name, label, 1)
        if default is not None:
            pt.setDefaultValue((float(default),))
        set_range(pt, param)
        return pt

    if type_name in ("int", "uint", "uint64"):
        pt = hou.IntParmTemplate(name, label, 1)
        if default is not None:
            pt.setDefaultValue((int(default),))
        set_range(pt, param)
        return pt

    if type_name in ("color", "vector", "point", "normal", "node"):
        pt = hou.FloatParmTemplate(name, label, 3)
        if isinstance(default, (list, tuple)) and len(default) >= 3:
            pt.setDefaultValue(tuple(float(x) for x in default[:3]))
        if type_name == "color":
            pt.setLook(hou.parmLook.ColorSquare)
            pt.setNamingScheme(hou.parmNamingScheme.RGBA)
        return pt

    if type_name == "point2":
        pt = hou.FloatParmTemplate(name, label, 2)
        if isinstance(default, (list, tuple)) and len(default) >= 2:
            pt.setDefaultValue(tuple(float(x) for x in default[:2]))
        return pt

    if type_name == "matrix":
        return hou.FloatParmTemplate(name, label, 16)

    if type_name == "string":
        pt = hou.StringParmTemplate(name, label, 1)
        if default is not None:
            pt.setDefaultValue((str(default),))
        if socket["name"] == "filename":
            pt.setStringType(hou.stringParmType.FileReference)
        return pt

    if type_name == "closure":
        return None

    pt = hou.FloatParmTemplate(name, label, 1)
    if default is not None and not isinstance(default, (list, tuple)):
        pt.setDefaultValue((float(default),))
    return pt


def hide_condition(param, parm_names, all_params):
    """Houdini hidewhen condition for the Blender visibility conditions of a parameter."""
    conditions = param["hidewhen"]
    if not conditions:
        return ""
    keys = set(k for c in conditions for k in c)
    if len(keys) == 1:
        # Use the shorter form: hide for these values, or hide unless one of the others.
        key = keys.pop()
        hidden = [c[key] for c in conditions]
        menu = next(p["menu"] for p in all_params if p["socket"]["name"] == key)
        shown = [token for token, _ in menu if token not in hidden]
        parm = parm_names[key]
        if len(shown) < len(hidden):
            return "{ " + " ".join("%s != %s" % (parm, token) for token in shown) + " }"
        return " ".join("{ %s == %s }" % (parm, token) for token in hidden)
    return " ".join(
        "{ " + " ".join("%s == %s" % (parm_names[k], v) for k, v in sorted(c.items())) + " }"
        for c in conditions)


def node_params(node_data, layouts, problems):
    """Label, panels and the laid out parameters with unique parameter names."""
    if layouts:
        label, panels, params = blender_layout.node_layout(node_data, layouts, problems)
    else:
        label = blender_layout.title(node_data.get("label", node_data["name"]))
        panels = []
        params = [{"socket": s, "label": blender_layout.title(s.get("ui_name") or s["name"]),
                   "default": s.get("default"),
                   "menu": list((s.get("enum_values") or {}).items()) or None,
                   "range": None, "panel": None, "hidden": False, "hidewhen": [], "help": ""}
                  for s in node_data["inputs"]]
    used = set()
    for param in params:
        name = param["socket"]["name"]
        base = name
        i = 1
        while name in used:
            name = base + "_" + str(i)
            i += 1
        used.add(name)
        param["parm"] = name
        # Houdini parameter names cannot contain dots (tex_mapping.scale).
        param["parm_ui"] = sanitize_parm_name(name)
    return label, panels, params


def create_shader_signature(node_data, shader_name, label, params):
    """Build a ShaderSignature from the laid out parameters."""
    shader_inputs = []
    for param in params:
        ui_parm = create_parm_template(param["parm"], param)
        shader_inputs.append(shaderhda.ShaderParameter(
            param["parm"],
            vop_type(param["socket"]["type"]),
            ui_parm,
            None,
            True,
            shaderhda.ShaderParameterExtraInfos(),
        ))

    used_output_names = set()
    shader_outputs = []
    for out in node_data.get("outputs", []):
        name = sanitize_parm_name(out["name"])
        base = name
        i = 1
        while name in used_output_names:
            name = base + "_" + str(i)
            i += 1
        used_output_names.add(name)
        shader_outputs.append(shaderhda.ShaderParameter(
            name,
            vop_type(out["type"]),
            None,
            None,
            True,
            shaderhda.ShaderParameterExtraInfos(),
        ))

    return shaderhda.ShaderSignature(
        shader_name,
        "Cycles " + label,
        shader_name,
        shader_inputs,
        shader_outputs,
    )


def apply_layout(hda_def, label, panels, params):
    """Put panel parameters in collapsible folders, set visibility conditions and help."""
    group = hda_def.parmTemplateGroup()
    parm_names = {p["socket"]["name"]: p["parm_ui"] for p in params}
    layout = hou.ParmTemplateGroup()
    # The network editor lists inputs outside folders last (as "Other"), so with panels the
    # top parameters go in a folder too, to keep them first like in Blender.
    top = None
    if any(p["panel"] for p in params):
        top = hou.FolderParmTemplate("folder_main", label, folder_type=hou.folderType.Simple)
    folders = {}
    placed = set()
    for param in params:
        pt = group.find(param["parm_ui"])
        if pt is None:
            continue
        condition = hide_condition(param, parm_names, params)
        if condition:
            pt.setConditional(hou.parmCondType.HideWhen, condition)
        if param["hidden"]:
            pt.hide(True)
        if param["help"]:
            pt.setHelp(param["help"])
        placed.add(pt.name())
        if param["panel"]:
            folder = folders.get(param["panel"])
            if folder is None:
                folder = hou.FolderParmTemplate(
                    "folder_" + sanitize_parm_name(param["panel"]).lower(),
                    param["panel"], folder_type=hou.folderType.Collapsible)
                folders[param["panel"]] = folder
            folder.addParmTemplate(pt)
        elif top:
            top.addParmTemplate(pt)
        else:
            layout.append(pt)
    if top and top.parmTemplates():
        layout.append(top)
    for panel in panels:
        if panel in folders:
            layout.append(folders[panel])
    # Anything shaderhda added that is not a socket parameter.
    for pt in group.entries():
        if pt.name() not in placed:
            layout.append(pt)
    hda_def.setParmTemplateGroup(layout)


def create_hda(node_data, install_dir, layouts, problems):
    """Create an HDA for a single Cycles node."""
    node_name = node_data["name"]
    shader_name = "cycles_" + node_name
    label, panels, params = node_params(node_data, layouts, problems)

    shader_hda = shaderhda.Shader(shader_name)

    if not shader_hda.shaderName():
        shader_hda.setShaderName(shader_name)

    if not shader_hda.hasHdaLabel():
        shader_hda.setHdaLabel("Cycles " + label)

    if not shader_hda.vopnetMask():
        shader_hda.setVopnetMask("cycles")

    if not shader_hda.renderMask():
        shader_hda.setRenderMask("cycles")

    shader_hda.setShaderType("generic")
    shader_hda.setToolKeywords(["cycles"])
    shader_hda.setTabSubMenus([blender_layout.tab_menu(node_data, layouts)])

    signature = create_shader_signature(node_data, shader_name, label, params)
    shader_hda.signatureSet().addSignature(signature)
    shader_hda.signatureSet().sortSignatures()
    shader_hda.signatureSet().setBestDefaultSignature()

    hda_filename = (shader_name + ".hda").replace("cycles_", "cycles-")
    hda_path = os.path.join(install_dir, hda_filename)
    os.makedirs(install_dir, exist_ok=True)

    shader_hda.addHDAToFile(hda_path, False)

    # The definition must be installed to edit its parameters.
    hou.hda.installFile(hda_path, change_oplibraries_file=False)
    hda_defs = [d for d in hou.hda.definitionsInFile(hda_path)
                if d.nodeTypeName() == shader_name]
    if not hda_defs:
        raise RuntimeError("No definition for %s in %s" % (shader_name, hda_path))
    hda_def = hda_defs[0]
    hda_def.setIcon("$CYCLES_ICON")
    if layouts:
        apply_layout(hda_def, label, panels, params)
    hda_def.save(hda_path)

    log("Created: " + hda_path)
    return params


def is_internal_node(name):
    """Nodes that Cycles creates itself and that are not useful to create by hand: type
    conversions inserted between sockets of different types, and the graph output."""
    return name.startswith("convert_") or name == "output"


def process_all(json_path, install_dir, layouts_path, defaults_path):
    with open(json_path, "r") as f:
        data = json.load(f)
    layouts = None
    if layouts_path:
        with open(layouts_path, "r") as f:
            layouts = json.load(f)
        log("Blender layouts from %s (Blender %s)" % (layouts_path, layouts["blender_version"]))

    nodes = [n for n in data.get("nodes", []) if not is_internal_node(n.get("name", ""))]
    log("Loaded %d nodes from %s" % (len(nodes), json_path))

    # Unload previously installed Cycles HDAs (e.g. from the Houdini package of an earlier
    # install), otherwise shaderhda starts from their old definitions.
    vop_types = hou.vopNodeTypeCategory().nodeTypes()
    loaded = set()
    for node_data in nodes:
        node_type = vop_types.get("cycles_" + node_data["name"])
        if node_type:
            for definition in node_type.allInstalledDefinitions():
                loaded.add(definition.libraryFilePath())
    for path in sorted(loaded):
        hou.hda.uninstallFile(path, change_oplibraries_file=False)
    if loaded:
        log("Unloaded %d previously installed HDA files" % len(loaded))

    successful = []
    failed = []
    problems = []
    defaults = {}

    for node_data in nodes:
        name = node_data.get("name", "<unknown>")
        try:
            params = create_hda(node_data, install_dir, layouts, problems)
            changed = blender_layout.changed_defaults(node_data, params)
            if changed:
                defaults[name] = changed
            successful.append(name)
        except Exception as e:
            log("Failed %s: %s" % (name, str(e)))
            traceback.print_exc()
            failed.append(name)

    for problem in problems:
        log("Layout: " + problem)

    if defaults_path:
        os.makedirs(os.path.dirname(os.path.abspath(defaults_path)), exist_ok=True)
        with open(defaults_path, "w", newline="\n") as f:
            json.dump(defaults, f, indent=1, sort_keys=True)
            f.write("\n")
        log("Wrote %d node defaults to %s" % (len(defaults), defaults_path))

    log("=" * 60)
    log("PROCESSING SUMMARY")
    log("=" * 60)
    log("Total: %d" % len(nodes))
    log("Successful: %d" % len(successful))
    log("Failed: %d" % len(failed))
    if failed:
        for name in failed:
            log("  - " + name)
    return not failed


def parse_args():
    parser = argparse.ArgumentParser(
        description="Generate Cycles shader HDAs from node registry JSON"
    )
    parser.add_argument("--json", required=True, help="Path to shader_nodes.json")
    parser.add_argument("--hda-install-dir", required=True, help="Output directory for .hda files")
    parser.add_argument("--layouts", help="Path to blender_node_layouts.json")
    parser.add_argument("--defaults-out", help="Path to write the shader node defaults to")
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
        return 1

    ok = process_all(json_path, install_dir, args.layouts, args.defaults_out)
    log("Conversion complete")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
