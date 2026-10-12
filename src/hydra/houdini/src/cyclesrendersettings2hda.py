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

    settings = subnet.createNode("rendersettings", "settings")
    settings.setInput(0, var)
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
