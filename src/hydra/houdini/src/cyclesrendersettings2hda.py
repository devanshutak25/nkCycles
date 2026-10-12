#!/usr/bin/env hython
"""
Generate the Cycles Render Settings LOP.

Usage:
    hython cyclesrendersettings2hda.py --json <render_settings.json> --ds-dir <soho/parameters>
        --hda-install-dir <output_dir>

Like the Karma Render Settings LOP, this creates the render settings, render product and render
var prims for rendering with Cycles in one node: output picture, camera and resolution at the top,
and the Cycles render settings below, laid out like Blender's Cycles render properties (the same
layout as the Cycles tab of the Render Settings LOP, see rendersettings2ds.py). Unlike that tab,
every setting is authored, so the node fully defines the render.

Inside, it is a Render Var, Render Product and Render Settings LOP with their parameters
referencing the parameters of the node. --ds-dir is the directory with HdCyclesPlugin_Global.ds,
which provides the Cycles parameters of the inner Render Settings LOP.
"""

import argparse
import os
import sys

import hou

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rendersettings2ds  # noqa: E402

NODE_TYPE = "cyclesrendersettings"
LABEL = "Cycles Render Settings"


def log(message):
    print("[cyclesrendersettings2hda] " + message)


TOOL_XML = """<?xml version="1.0" encoding="UTF-8"?>
<shelfDocument>
  <tool name="$HDA_DEFAULT_TOOL" label="$HDA_LABEL" icon="$HDA_ICON">
    <toolMenuContext name="viewer">
      <contextNetType>LOP</contextNetType>
    </toolMenuContext>
    <toolMenuContext name="network">
      <contextOpType>$HDA_TABLE_AND_NAME</contextOpType>
    </toolMenuContext>
    <toolSubmenu>Cycles</toolSubmenu>
    <toolSubmenu>Render</toolSubmenu>
    <script scriptType="python"><![CDATA[import loptoolutils

loptoolutils.genericTool(kwargs, '$HDA_NAME')]]></script>
  </tool>
</shelfDocument>
"""


def ensure_cycles_parms(node, ds_dir):
    """Make sure the Render Settings LOP has the Cycles tab, also when Houdini does not know
    the Cycles renderer yet in this session."""
    probe = hou.text.encodeParm("cycles:samples")
    if node.parm(probe) is None:
        import loputils
        loputils.addDialogScriptFolder(
            os.path.join(ds_dir, "HdCyclesPlugin_Global.ds").replace("\\", "/"), node, "Cycles")
    if node.parm(probe) is None:
        raise RuntimeError("Render Settings LOP has no Cycles parameters")


# Render passes, laid out like Blender's view layer passes: (folder, [(row, [(parm, label,
# [render var (name, source, source type, data type)])])]). Data and light passes have Blender's
# names. Light path expressions have the names of Karma's AOVs and include the albedo, like
# Karma's; Blender's light passes are divided by the color pass.
AOV_FOLDERS = [
    ("Data", [
        ("Include", [
            ("aov_depth", "Z", [("Depth", "depth", "raw", "float")]),
            ("aov_mist", "Mist", [("Mist", "mist", "raw", "float")]),
            ("aov_position", "Position", [("Position", "position", "raw", "color3f")]),
            ("aov_normal", "Normal", [("Normal", "normal", "raw", "color3f")]),
            ("aov_vector", "Vector", [("Vector", "motion", "raw", "color4f")]),
            ("aov_uv", "UV", [("UV", "uv", "raw", "color3f")]),
            ("aov_denoising", "Denoising Data", [
                ("Denoising Normal", "denoising_normal", "raw", "color3f"),
                ("Denoising Albedo", "denoising_albedo", "raw", "color3f"),
                ("Denoising Depth", "denoising_depth", "raw", "float")]),
        ]),
        ("Indexes", [
            ("aov_object_index", "Object Index", [("IndexOB", "object_id", "raw", "float")]),
        ]),
        ("Debug", [
            ("aov_sample_count", "Sample Count",
             [("Debug Sample Count", "sample_count", "raw", "float")]),
        ]),
    ]),
    ("Light", [
        ("Diffuse", [
            ("aov_diffuse_direct", "Direct", [("DiffDir", "diffuse_direct", "raw", "color3f")]),
            ("aov_diffuse_indirect", "Indirect",
             [("DiffInd", "diffuse_indirect", "raw", "color3f")]),
            ("aov_diffuse_color", "Color", [("DiffCol", "diffuse_color", "raw", "color3f")]),
        ]),
        ("Glossy", [
            ("aov_glossy_direct", "Direct", [("GlossDir", "glossy_direct", "raw", "color3f")]),
            ("aov_glossy_indirect", "Indirect",
             [("GlossInd", "glossy_indirect", "raw", "color3f")]),
            ("aov_glossy_color", "Color", [("GlossCol", "glossy_color", "raw", "color3f")]),
        ]),
        ("Transmission", [
            ("aov_transmission_direct", "Direct",
             [("TransDir", "transmission_direct", "raw", "color3f")]),
            ("aov_transmission_indirect", "Indirect",
             [("TransInd", "transmission_indirect", "raw", "color3f")]),
            ("aov_transmission_color", "Color",
             [("TransCol", "transmission_color", "raw", "color3f")]),
        ]),
        ("Volume", [
            ("aov_volume_direct", "Direct", [("VolumeDir", "volume_direct", "raw", "color3f")]),
            ("aov_volume_indirect", "Indirect",
             [("VolumeInd", "volume_indirect", "raw", "color3f")]),
        ]),
        ("Other", [
            ("aov_emission", "Emission", [("Emit", "emission", "raw", "color3f")]),
            ("aov_environment", "Environment", [("Env", "background", "raw", "color3f")]),
            ("aov_ao", "Ambient Occlusion", [("AO", "ao", "raw", "color3f")]),
            ("aov_shadow_catcher", "Shadow Catcher",
             [("Shadow Catcher", "shadow_catcher", "raw", "color3f")]),
        ]),
    ]),
    ("Light Path Expressions", [
        ("Diffuse", [
            ("aov_lpe_combineddiffuse", "Combined",
             [("combineddiffuse", "C<RD>.*L", "lpe", "color3f")]),
            ("aov_lpe_directdiffuse", "Direct", [("directdiffuse", "C<RD>L", "lpe", "color3f")]),
            ("aov_lpe_indirectdiffuse", "Indirect",
             [("indirectdiffuse", "C<RD>.+L", "lpe", "color3f")]),
        ]),
        ("Glossy Reflection", [
            ("aov_lpe_combinedglossyreflection", "Combined",
             [("combinedglossyreflection", "C<RG>.*L", "lpe", "color3f")]),
            ("aov_lpe_directglossyreflection", "Direct",
             [("directglossyreflection", "C<RG>L", "lpe", "color3f")]),
            ("aov_lpe_indirectglossyreflection", "Indirect",
             [("indirectglossyreflection", "C<RG>.+L", "lpe", "color3f")]),
        ]),
        ("Glossy Transmission", [
            ("aov_lpe_glossytransmission", "Combined",
             [("glossytransmission", "C<TG>.*L", "lpe", "color3f")]),
        ]),
        ("Volume", [
            ("aov_lpe_combinedvolume", "Combined", [("combinedvolume", "CV.*L", "lpe", "color3f")]),
            ("aov_lpe_directvolume", "Direct", [("directvolume", "CVL", "lpe", "color3f")]),
            ("aov_lpe_indirectvolume", "Indirect",
             [("indirectvolume", "CV.+L", "lpe", "color3f")]),
        ]),
        ("Other", [
            ("aov_lpe_directemission", "Direct Emission",
             [("directemission", "CO", "lpe", "color3f")]),
            ("aov_lpe_albedo", "Albedo", [("albedo", "CDA", "lpe", "color3f")]),
        ]),
    ]),
]

# Light path expressions using the same Cycles pass as a Blender light pass: only one of them can
# be rendered, with or without the albedo.
AOV_SHARED_PASS = {
    "aov_lpe_directdiffuse": "aov_diffuse_direct",
    "aov_lpe_indirectdiffuse": "aov_diffuse_indirect",
    "aov_lpe_directglossyreflection": "aov_glossy_direct",
    "aov_lpe_indirectglossyreflection": "aov_glossy_indirect",
}

# Python Script LOP inside the node: authors a render var for every enabled pass and shader AOV
# and adds it to the render product.
AOV_SCRIPT = """import hou
from pxr import Sdf, Tf, UsdRender

PASSES = %r

top = hou.pwd().parent()
stage = hou.pwd().editableStage()
vars_path = top.evalParm("rendervarsparentprimpath")
product = UsdRender.Product(stage.GetPrimAtPath(
    top.evalParm("renderproductsparentprimpath") + "/renderproduct"))
ordered_vars = product.GetOrderedVarsRel()


def add_var(name, source, source_type, data_type):
    var = UsdRender.Var.Define(stage, vars_path + "/" + Tf.MakeValidIdentifier(name))
    var.CreateSourceNameAttr(source)
    var.CreateSourceTypeAttr(source_type)
    var.CreateDataTypeAttr(data_type)
    prim = var.GetPrim()
    prim.CreateAttribute("driver:parameters:aov:husk:name", Sdf.ValueTypeNames.String).Set(name)
    prim.CreateAttribute("driver:parameters:aov:husk:format", Sdf.ValueTypeNames.String).Set(
        data_type)
    ordered_vars.AddTarget(var.GetPath())


for parm, render_vars in PASSES:
    if top.evalParm(parm):
        for render_var in render_vars:
            add_var(*render_var)

for i in range(1, top.evalParm("shader_aovs") + 1):
    name = top.evalParm("shader_aov_name%%d" %% i)
    if name:
        color = top.parm("shader_aov_type%%d" %% i).evalAsString() == "color"
        add_var(name, name, "raw", "color3f" if color else "float")

# Image filter lists (slap comps) husk applies to the rendered AOVs
filters = top.evalParm("imagefilters").split()
if filters:
    settings = stage.OverridePrim(top.evalParm("primpath"))
    settings.CreateRelationship("husk:orderedImageFilters").SetTargets(
        [Sdf.Path(path) for path in filters])
"""


def aov_passes():
    """(parm, render vars) of every pass toggle."""
    return [(parm, render_vars) for _, rows in AOV_FOLDERS for _, toggles in rows
            for parm, _, render_vars in toggles]


def aov_parameters(passes_folder):
    """Add the pass toggles and shader AOVs to the Passes folder."""
    for folder_label, rows in AOV_FOLDERS:
        folder = hou.FolderParmTemplate(
            "folder_aov_" + folder_label.lower().replace(" ", "_"), folder_label,
            folder_type=hou.folderType.Collapsible)
        if folder_label == "Light Path Expressions":
            folder.addParmTemplate(hou.LabelParmTemplate(
                "aov_lpe_note", "", column_labels=(
                    "Karma's AOVs, with the albedo. Cycles has no LPE engine: these are the "
                    "matching Cycles passes.",)))
        for row_label, toggles in rows:
            label = hou.LabelParmTemplate("aov_label_" + toggles[0][0], row_label,
                                          column_labels=("",))
            label.setJoinWithNext(True)
            folder.addParmTemplate(label)
            for i, (parm, toggle_label, render_vars) in enumerate(toggles):
                names = ", ".join(v[0] for v in render_vars)
                toggle = hou.ToggleParmTemplate(
                    parm, toggle_label, default_value=False,
                    help="Render the %s render var%s (%s)." % (
                        names, "s" if len(render_vars) > 1 else "",
                        ", ".join(v[1] for v in render_vars)))
                if parm in AOV_SHARED_PASS:
                    toggle.setConditional(hou.parmCondType.DisableWhen,
                                          "{ %s == 1 }" % AOV_SHARED_PASS[parm])
                toggle.setJoinWithNext(i < len(toggles) - 1)
                folder.addParmTemplate(toggle)
        passes_folder.addParmTemplate(folder)

    shader = hou.FolderParmTemplate("folder_aov_shader", "Shader AOV",
                                    folder_type=hou.folderType.Collapsible)
    aovs = hou.FolderParmTemplate("shader_aovs", "AOVs",
                                  folder_type=hou.folderType.MultiparmBlock)
    aovs.setHelp("Render vars for AOV Output shader nodes, matched by name.")
    name = hou.StringParmTemplate("shader_aov_name#", "Name", 1, default_value=("AOV",))
    name.setJoinWithNext(True)
    aovs.addParmTemplate(name)
    aovs.addParmTemplate(hou.MenuParmTemplate(
        "shader_aov_type#", "Type", ("color", "value"), ("Color", "Value")))
    shader.addParmTemplate(aovs)
    passes_folder.addParmTemplate(shader)


def string_ref(parm_name):
    return '`chs("../%s")`' % parm_name


def build_contents(subnet, sockets, ds_dir):
    # Same prims as the Karma Render Settings LOP: Products/renderproduct, then Vars/beauty.
    product = subnet.createNode("renderproduct", "renderproduct")
    product.setInput(0, subnet.indirectInputs()[0])
    product.parm("primpath").set('`chs("../renderproductsparentprimpath")`/renderproduct')
    product.parm("productName").set(string_ref("picture"))
    product.parm("orderedVars").set('`chs("../rendervarsparentprimpath")`/beauty')

    var = subnet.createNode("rendervar", "beauty")
    var.setInput(0, product)
    var.parm("primpath").set('`chs("../rendervarsparentprimpath")`/beauty')
    var.parm("sourceName").set("color")
    var.parm("dataType").set("color4f")
    var.parm("sourceType").set("raw")
    # Write RGBA channels (husk defaults to one float channel).
    var.parm(hou.text.encodeParm("driver:parameters:aov:husk:format")).set("color4f")
    var.parm(hou.text.encodeParm("driver:parameters:aov:husk:name")).set("color")

    aovs = subnet.createNode("pythonscript", "aovs")
    aovs.setInput(0, var)
    aovs.parm("python").set(AOV_SCRIPT % (aov_passes(),))

    settings = subnet.createNode("rendersettings", "settings")
    settings.setInput(0, aovs)
    settings.parm("primpath").set(string_ref("primpath"))
    settings.parm("camera").set(string_ref("camera"))
    settings.parm("res_mode").set("manual")
    # The node has the same camera and resolution parameters as the Render Settings LOP.
    # Automatic resolution modes lock the computed component.
    for parm in settings.parmTuple("resolution"):
        parm.lock(False)
        parm.setExpression('ch("../%s")' % parm.name())
    settings.parm("pixelAspectRatio").setExpression('ch("../pixelAspectRatio")')
    settings.parm("products").set('`chs("../renderproductsparentprimpath")`/renderproduct')

    ensure_cycles_parms(settings, ds_dir)
    for setting in rendersettings2ds.settings():
        socket = sockets[setting["name"]]
        parm_name = hou.text.encodeParm(setting["name"])
        settings.parm(hou.text.encodeParm(setting["name"] + "_control")).set("set")
        parm = settings.parm(parm_name)
        if socket["type"] == "menu":
            parm.set(string_ref(parm_name))
        else:
            parm.setExpression('ch("../%s")' % parm_name)

    outputs = [c for c in subnet.children() if c.type().name() == "output"]
    output = outputs[0] if outputs else subnet.createNode("output", "output0")
    output.setInput(0, settings)
    output.setDisplayFlag(True)
    subnet.layoutChildren()


def top_parameters(sockets, reference):
    """Parameters of the node: prims, output, camera, resolution, then the Cycles settings."""
    group = hou.ParmTemplateGroup()

    prims = hou.FolderParmTemplate("folder_prims", "Render Settings Primitives",
                                   folder_type=hou.folderType.Collapsible)
    prims.addParmTemplate(hou.StringParmTemplate(
        "primpath", "Render Settings Path", 1, default_value=("/Render/$OS",),
        help="Path of the render settings primitive to create."))
    prims.addParmTemplate(hou.StringParmTemplate(
        "renderproductsparentprimpath", "Render Products Parent Path", 1,
        default_value=("/Render/$OS/Products",),
        help="Parent path of the render product primitive."))
    prims.addParmTemplate(hou.StringParmTemplate(
        "rendervarsparentprimpath", "Render Vars Parent Path", 1,
        default_value=("/Render/$OS/Vars",),
        help="Parent path of the render var primitives."))
    group.append(prims)

    picture = hou.StringParmTemplate(
        "picture", "Output Picture", 1,
        default_value=("$HIP/render/$HIPNAME.$OS.$F4.exr",),
        string_type=hou.stringParmType.FileReference,
        file_type=hou.fileType.Image,
        help="Image file to render to.")
    group.append(picture)

    image_filters = hou.StringParmTemplate(
        "imagefilters", "Image Filters", 1,
        help="Image filter list primitives (made by the Image Filter LOP) that husk applies to "
             "the rendered AOVs, like a slap comp.")
    image_filters.setTags({"script_action": "import loputils\n"
                                            "loputils.selectPrimsInParm(kwargs, True)",
                           "script_action_icon": "BUTTONS_reselect",
                           "script_action_help": "Select image filter list primitives."})
    group.append(image_filters)

    # Camera and resolution like the Render Settings LOP, including its prim and resolution
    # choosers.
    for name in ("camera", "res_mode", "resolution", "resolutionMenu", "pixelAspectRatio"):
        template = reference.parm(name).parmTemplate() if reference.parm(name) else \
            reference.parmTuple(name).parmTemplate()
        template = template.clone()
        template.setConditional(hou.parmCondType.DisableWhen, "")
        group.append(template)

    # The Cycles settings, in Blender's layout.
    ds = rendersettings2ds.Writer(sockets, "lop").generate()
    cycles = hou.ParmTemplateGroup()
    cycles.setToDialogScript(ds)
    for entry in cycles.entries():
        if entry.name() == "cycles_passes_1":
            entry = entry.clone()
            aov_parameters(entry)
        group.append(entry)
    return group


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--json", required=True, help="Path to render_settings.json")
    parser.add_argument("--ds-dir", required=True,
                        help="Directory with HdCyclesPlugin_Global.ds (soho/parameters)")
    parser.add_argument("--hda-install-dir", required=True, help="Directory to write the HDA to")
    args = parser.parse_args()

    sockets = rendersettings2ds.load_sockets(args.json)
    os.makedirs(args.hda_install_dir, exist_ok=True)
    hda_path = os.path.join(args.hda_install_dir, "cycles-rendersettings.hda")

    # Start from scratch, also when an earlier install of the node is loaded.
    lop_type = hou.lopNodeTypeCategory().nodeTypes().get(NODE_TYPE)
    if lop_type:
        for definition in lop_type.allInstalledDefinitions():
            hou.hda.uninstallFile(definition.libraryFilePath(), change_oplibraries_file=False)
    if os.path.exists(hda_path):
        os.remove(hda_path)

    stage = hou.node("/stage")
    reference = stage.createNode("rendersettings")
    subnet = stage.createNode("subnet", NODE_TYPE)
    build_contents(subnet, sockets, args.ds_dir)

    hda_node = subnet.createDigitalAsset(
        name=NODE_TYPE, hda_file_name=hda_path, description=LABEL,
        min_num_inputs=0, max_num_inputs=1, ignore_external_references=True)
    definition = hda_node.type().definition()
    definition.setParmTemplateGroup(top_parameters(sockets, reference))
    definition.setIcon("$CYCLES_ICON")
    definition.addSection("Tools.shelf", TOOL_XML)
    definition.setExtraFileOption("UnlockOnCreate", False)
    definition.save(hda_path)

    # Check that the node creates the render prims with the expected settings.
    hou.hda.installFile(hda_path, change_oplibraries_file=False)
    test = stage.createNode(NODE_TYPE)
    test.parm(hou.text.encodeParm("cycles:samples")).set(64)
    stage_prims = test.stage()
    settings_prim = stage_prims.GetPrimAtPath("/Render/%s" % test.name())
    if not settings_prim:
        raise RuntimeError("Render settings prim not created")
    samples = settings_prim.GetAttribute("cycles:samples").Get()
    if samples != 64:
        raise RuntimeError("cycles:samples is %r instead of 64" % samples)
    for path, type_name in (("Products/renderproduct", "RenderProduct"),
                            ("Vars/beauty", "RenderVar")):
        prim = stage_prims.GetPrimAtPath("/Render/%s/%s" % (test.name(), path))
        if not prim or prim.GetTypeName() != type_name:
            raise RuntimeError("%s prim %s not created" % (type_name, path))
    test.parm("aov_diffuse_direct").set(True)
    test.parm("aov_lpe_combineddiffuse").set(True)
    test.parm("shader_aovs").set(1)
    stage_prims = test.stage()
    product = stage_prims.GetPrimAtPath("/Render/%s/Products/renderproduct" % test.name())
    targets = [str(t).rsplit("/", 1)[1]
               for t in product.GetRelationship("orderedVars").GetTargets()]
    if targets != ["beauty", "DiffDir", "combineddiffuse", "AOV"]:
        raise RuntimeError("Unexpected render vars %r" % targets)
    log("Wrote %s (%d settings)" % (hda_path, len(list(rendersettings2ds.settings()))))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        import traceback
        traceback.print_exc()
        log("Error: %s" % e)
        sys.exit(1)
