#!/usr/bin/env hython
"""
Generate the Houdini render settings UI for Cycles from render_settings.json.

Usage:
    hython rendersettings2ds.py --json <render_settings.json> --ds-install-dir <output_dir>

Houdini looks for dialog scripts named after the render delegate plugin in
$HOUDINI_PATH/soho/parameters:

  HdCyclesPlugin_Global.ds    Cycles tab of the Render Settings LOP.
  HdCyclesPlugin_Viewport.ds  Cycles section of the viewport display options.
  HdCyclesPlugin_Geometry.ds  Cycles tab of the Render Geometry Settings LOP (per object).

Each parameter is a "cycles:..." render setting forwarded to the delegate.
Types, defaults and menus come from the Cycles node sockets dumped to
render_settings.json, so the UI always shows what the delegate uses when a
setting is not authored. Labels and layout follow Blender's Cycles render
properties, with Blender's panels as collapsible folders. The same layout is
used by the Cycles Render Settings LOP (cyclesrendersettings2hda.py).
"""

import argparse
import json
import os
import sys

import hou


def log(message):
    print("[rendersettings2ds] " + message)


# Settings that are not node sockets, but session parameters of the delegate.
SESSION_SETTINGS = {
    "cycles:device": {
        "type": "menu",
        "default": "AUTO",
        "enum_values": {
            "AUTO": "Automatic (Best GPU)",
            "OPTIX": "GPU - OptiX",
            "CUDA": "GPU - CUDA",
            "HIP": "GPU - HIP",
            "ONEAPI": "GPU - oneAPI",
            "METAL": "GPU - Metal",
            "CPU": "CPU",
        },
    },
    "cycles:threads": {"type": "int", "default": 0},
    "cycles:samples": {"type": "int", "default": 1024},
    "cycles:time_limit": {"type": "float", "default": 0.0},
    "cycles:sample_offset": {"type": "int", "default": 0},
}

# Menu labels and items for socket enums, where the title cased token is not good enough.
# Items missing here are left out of the menu.
MENU_ITEMS = {
    "cycles:integrator:denoiser_type": {
        "openimagedenoise": "OpenImageDenoise",
        "optix": "OptiX",
    },
    "cycles:integrator:denoiser_prefilter": {
        "none": "None",
        "fast": "Fast",
        "accurate": "Accurate",
    },
    "cycles:integrator:denoiser_quality": {
        "high": "High",
        "balanced": "Balanced",
        "fast": "Fast",
    },
    "cycles:integrator:sampling_pattern": {
        "tabulated_sobol": "Tabulated Sobol",
        "sobol_burley": "Sobol-Burley",
        "blue_noise_pure": "Blue-Noise (pure)",
        "blue_noise_round": "Blue-Noise (round)",
        "blue_noise_first": "Blue-Noise (first)",
    },
    "cycles:film:filter_type": {
        "box": "Box",
        "gaussian": "Gaussian",
        "blackman_harris": "Blackman-Harris",
    },
}


def S(name, label, help, range=None, viewport=False, toolbar_icon=None, hidewhen=None,
      disablewhen=None):
    """A single setting in the layout."""
    return {
        "name": name,
        "label": label,
        "help": help,
        "range": range,
        "viewport": viewport,
        "toolbar_icon": toolbar_icon,
        "hidewhen": hidewhen,
        "disablewhen": disablewhen,
    }


def P(label, items, open=False, toggle=None):
    """A panel of settings and sub-panels, like a Blender properties panel.

    toggle is the setting shown in the panel header in Blender. It is the first parameter of the
    folder, and the other settings in the panel are disabled when it is off.
    """
    return {"panel": label, "items": items, "open": open, "toggle": toggle}


# Layout of the settings, following Blender's Cycles render properties.
LAYOUT = [
    P("Device", [
        S("cycles:device", "Device",
          "Device to render with. Automatic uses the best available GPU (OptiX, then CUDA, "
          "HIP, oneAPI, Metal) and falls back to the CPU. Changing it restarts the render.",
          viewport=True),
    ], open=True),
    P("Sampling", [
        P("Render", [
            S("cycles:integrator:use_adaptive_sampling", "Noise Threshold",
              "Automatically reduce the number of samples per pixel based on estimated noise "
              "level.",
              viewport=True),
            S("cycles:integrator:adaptive_threshold", "Threshold",
              "Noise level step to stop sampling at, lower values reduce noise at the cost of "
              "render time. 0 for automatic setting based on the number of samples.",
              range=(0, 1, True, True), viewport=True,
              disablewhen="{ cycles:integrator:use_adaptive_sampling == 0 }"),
            S("cycles:samples", "Max Samples",
              "Number of samples to render for each pixel.",
              range=(1, 4096, True, False), viewport=True),
            S("cycles:integrator:adaptive_min_samples", "Min Samples",
              "Minimum number of samples before adaptive sampling stops a pixel. 0 for automatic "
              "setting based on the noise threshold.",
              range=(0, 4096, True, False), viewport=True,
              disablewhen="{ cycles:integrator:use_adaptive_sampling == 0 }"),
            S("cycles:time_limit", "Time Limit",
              "Limit the render time in seconds, excluding synchronization time. 0 disables the "
              "limit.",
              range=(0, 600, True, False)),
            P("Denoise", [
                S("cycles:integrator:use_denoise", "Denoise",
                  "Denoise the rendered image.",
                  viewport=True, toolbar_icon="VIEW_display_denoise"),
                S("cycles:integrator:denoiser_type", "Denoiser",
                  "Denoiser to use. OptiX requires an NVIDIA GPU.",
                  viewport=True),
                S("cycles:integrator:denoiser_prefilter", "Prefilter",
                  "Prefilter of the guiding passes (albedo and normal). Accurate gives the best "
                  "detail, None should only be used when the guiding passes are noise free.",
                  viewport=True),
                S("cycles:integrator:denoiser_quality", "Quality",
                  "Quality of the OpenImageDenoise denoiser, balanced and fast trade quality for "
                  "speed.",
                  viewport=True,
                  hidewhen="{ cycles:integrator:denoiser_type != openimagedenoise }"),
                S("cycles:integrator:denoise_start_sample", "Start Sample",
                  "Sample to start denoising the interactive render at.",
                  range=(0, 64, True, False), viewport=True),
                S("cycles:integrator:denoise_use_gpu", "Use GPU",
                  "Run the denoiser on the GPU when available.",
                  viewport=True),
            ], toggle="cycles:integrator:use_denoise"),
        ], open=True),
        P("Path Guiding", [
            S("cycles:integrator:use_guiding", "Path Guiding",
              "Use path guiding for sampling paths. Path guiding incrementally learns the light "
              "distribution of the scene and guides paths into directions with high direct and "
              "indirect light contributions. Only supported on the CPU."),
            S("cycles:integrator:guiding_training_samples", "Training Samples",
              "The maximum number of samples used for training path guiding. 0 trains for all "
              "samples.",
              range=(0, 1024, True, False)),
            S("cycles:integrator:use_surface_guiding", "Surface",
              "Use guiding when sampling directions on a surface."),
            S("cycles:integrator:use_volume_guiding", "Volume",
              "Use guiding when sampling directions inside a volume."),
        ], toggle="cycles:integrator:use_guiding"),
        P("Advanced", [
            S("cycles:integrator:sampling_pattern", "Pattern",
              "Random sampling pattern used by the integrator."),
            S("cycles:integrator:seed", "Seed",
              "Seed value for the random number generator, to get different noise patterns.",
              range=(0, 100, True, False)),
            S("cycles:integrator:scrambling_distance", "Scrambling Distance",
              "Reduce the randomization between pixels to improve GPU rendering performance, at "
              "the cost of possible rendering artifacts if set too low.",
              range=(0, 1, True, True)),
            S("cycles:integrator:min_bounce", "Min Light Bounces",
              "Minimum number of light bounces. Setting this higher reduces noise in the first "
              "bounces, but can also be less efficient for more complex geometry like hair.",
              range=(0, 32, True, False)),
            S("cycles:integrator:transparent_min_bounce", "Min Transparent Bounces",
              "Minimum number of transparent bounces. Setting this higher reduces noise in the "
              "first bounces, but can also be less efficient for more complex geometry like "
              "hair.",
              range=(0, 32, True, False)),
            S("cycles:sample_offset", "Sample Offset",
              "Number of samples to skip when starting the render.",
              range=(0, 4096, True, False)),
        ]),
        P("Lights", [
            S("cycles:integrator:use_light_tree", "Light Tree",
              "Sample multiple lights more efficiently based on estimated contribution at every "
              "shading point."),
            S("cycles:integrator:light_sampling_threshold", "Light Threshold",
              "Probabilistically terminate light samples when the light contribution is below "
              "this threshold (more noise but faster rendering). 0 disables the test.",
              range=(0, 1, True, False)),
        ]),
    ], open=True),
    P("Light Paths", [
        P("Max Bounces", [
            S("cycles:integrator:max_bounce", "Total",
              "Total maximum number of bounces.",
              range=(0, 32, True, False), viewport=True),
            S("cycles:integrator:max_diffuse_bounce", "Diffuse",
              "Maximum number of diffuse reflection bounces, bounded by total maximum.",
              range=(0, 32, True, False), viewport=True),
            S("cycles:integrator:max_glossy_bounce", "Glossy",
              "Maximum number of glossy reflection bounces, bounded by total maximum.",
              range=(0, 32, True, False), viewport=True),
            S("cycles:integrator:max_transmission_bounce", "Transmission",
              "Maximum number of transmission bounces, bounded by total maximum.",
              range=(0, 32, True, False), viewport=True),
            S("cycles:integrator:max_volume_bounce", "Volume",
              "Maximum number of volumetric scattering events.",
              range=(0, 32, True, False), viewport=True),
            S("cycles:integrator:transparent_max_bounce", "Transparent",
              "Maximum number of transparent bounces. This is independent of maximum number of "
              "other bounces.",
              range=(0, 32, True, False), viewport=True),
        ], open=True),
        P("Clamping", [
            S("cycles:integrator:sample_clamp_direct", "Direct Light",
              "If non-zero, the maximum value for a direct sample, higher values will be scaled "
              "down to avoid too much noise and slow convergence at the cost of accuracy.",
              range=(0, 100, True, False)),
            S("cycles:integrator:sample_clamp_indirect", "Indirect Light",
              "If non-zero, the maximum value for an indirect sample, higher values will be "
              "scaled down to avoid too much noise and slow convergence at the cost of accuracy.",
              range=(0, 100, True, False)),
        ], open=True),
        P("Caustics", [
            S("cycles:integrator:filter_glossy", "Filter Glossy",
              "Adaptively blur glossy shaders after blurry bounces, to reduce noise at the cost "
              "of accuracy.",
              range=(0, 10, True, False)),
            S("cycles:integrator:caustics_reflective", "Reflective",
              "Use reflective caustics, resulting in a brighter image (more noise but added "
              "realism)."),
            S("cycles:integrator:caustics_refractive", "Refractive",
              "Use refractive caustics, resulting in a brighter image (more noise but added "
              "realism)."),
        ], open=True),
        P("Fast GI Approximation", [
            S("cycles:integrator:ao_bounces", "Bounces",
              "After this number of light bounces, use approximate global illumination. "
              "0 disables the approximation, which also needs a non-zero AO Factor.",
              range=(0, 16, True, False)),
            S("cycles:integrator:ao_factor", "AO Factor",
              "Factor for the approximate global illumination replacing the indirect light "
              "(Replace method). 0 disables the approximation.",
              range=(0, 1, True, False)),
            S("cycles:integrator:ao_additive_factor", "AO Additive Factor",
              "Factor for the approximate global illumination added to the indirect light "
              "(Add method).",
              range=(0, 1, True, False)),
            S("cycles:integrator:ao_distance", "AO Distance",
              "Distance from shading point to trace rays for the approximation.",
              range=(0, 100, True, False)),
        ]),
    ]),
    P("Volumes", [
        S("cycles:integrator:volume_ray_marching", "Biased",
          "Use the biased ray marching volume rendering instead of the unbiased null "
          "scattering method."),
        S("cycles:integrator:volume_step_rate", "Step Rate",
          "Globally adjust detail for volume rendering, on top of automatically estimated step "
          "size. Higher values reduce render time, lower values render with more detail.",
          range=(0.01, 100, True, False),
          hidewhen="{ cycles:integrator:volume_ray_marching == 0 }"),
        S("cycles:integrator:volume_max_steps", "Max Steps",
          "Maximum number of steps through the volume before giving up, to avoid extremely "
          "long render times with big objects or small step sizes.",
          range=(2, 65536, True, False),
          hidewhen="{ cycles:integrator:volume_ray_marching == 0 }"),
    ]),
    P("Film", [
        S("cycles:film:exposure", "Exposure",
          "Image brightness scale.",
          range=(0, 10, True, False)),
        P("Pixel Filter", [
            S("cycles:film:filter_type", "Type",
              "Pixel filter type."),
            S("cycles:film:filter_width", "Width",
              "Pixel filter width.",
              range=(0.01, 10, True, False),
              hidewhen="{ cycles:film:filter_type == box }"),
        ], open=True),
        P("Transparent", [
            S("cycles:background:transparent", "Transparent",
              "World background is transparent, for compositing the render over another "
              "background. When not set, the background is transparent unless the scene has a "
              "dome light."),
            S("cycles:background:transparent_glass", "Transparent Glass",
              "Render transmissive surfaces as transparent, for compositing glass over another "
              "background."),
            S("cycles:background:transparent_roughness_threshold", "Roughness Threshold",
              "For transparent transmission, keep surfaces with roughness above the threshold "
              "opaque.",
              range=(0, 1, True, True),
              disablewhen="{ cycles:background:transparent_glass == 0 }"),
        ], open=True, toggle="cycles:background:transparent"),
    ]),
    P("Performance", [
        P("Threads", [
            S("cycles:threads", "Threads",
              "Number of CPU threads to use. 0 uses all cores.",
              range=(0, 64, True, False)),
        ], open=True),
    ]),
]

# USD value type, Houdini parameter type and Edit Properties control type per socket type.
TYPES = {
    "boolean": ("bool", "toggle", "bool"),
    "int": ("int", "int", "int"),
    "float": ("float", "float", "float"),
    "menu": ("token", "string", "string"),
}


# Per-object settings, authored as constant primvars by the Render Geometry Settings LOP. The
# delegate reads "cycles:object:visible_<ray>" as ray visibility and the others as the Cycles
# object socket of the same name.
GEOMETRY_SETTINGS = {
    "primvars:cycles:object:visible_camera": {"type": "boolean", "default": True},
    "primvars:cycles:object:visible_diffuse": {"type": "boolean", "default": True},
    "primvars:cycles:object:visible_glossy": {"type": "boolean", "default": True},
    "primvars:cycles:object:visible_transmission": {"type": "boolean", "default": True},
    "primvars:cycles:object:visible_volume_scatter": {"type": "boolean", "default": True},
    "primvars:cycles:object:visible_shadow": {"type": "boolean", "default": True},
    "primvars:cycles:object:is_shadow_catcher": {"type": "boolean", "default": False},
    "primvars:cycles:object:use_holdout": {"type": "boolean", "default": False},
    "primvars:cycles:object:is_caustics_caster": {"type": "boolean", "default": False},
    "primvars:cycles:object:is_caustics_receiver": {"type": "boolean", "default": False},
    "primvars:cycles:object:shadow_terminator_geometry_offset": {"type": "float",
                                                                 "default": 0.1},
    "primvars:cycles:object:shadow_terminator_shading_offset": {"type": "float",
                                                                "default": 0.0},
    "primvars:cycles:object:ao_distance": {"type": "float", "default": 0.0},
}

# Layout of the per-object settings, following Blender's Cycles object Visibility panel.
GEOMETRY_LAYOUT = [
    P("Visibility", [
        P("Ray Visibility", [
            S("primvars:cycles:object:visible_camera", "Camera",
              "Object visibility for camera rays."),
            S("primvars:cycles:object:visible_diffuse", "Diffuse",
              "Object visibility for diffuse reflection rays."),
            S("primvars:cycles:object:visible_glossy", "Glossy",
              "Object visibility for glossy reflection rays."),
            S("primvars:cycles:object:visible_transmission", "Transmission",
              "Object visibility for transmission rays."),
            S("primvars:cycles:object:visible_volume_scatter", "Volume Scatter",
              "Object visibility for volume scatter rays."),
            S("primvars:cycles:object:visible_shadow", "Shadow",
              "Object visibility for shadow rays."),
        ], open=True),
        P("Mask", [
            S("primvars:cycles:object:is_shadow_catcher", "Shadow Catcher",
              "Only render shadows and reflections on this object, for compositing renders "
              "into real footage."),
            S("primvars:cycles:object:use_holdout", "Holdout",
              "Render a hole in the image with transparent alpha instead of the object."),
        ], open=True),
        P("Caustics", [
            S("primvars:cycles:object:is_caustics_caster", "Cast Shadow Caustics",
              "With Manifold Next Event Estimation, cast shadow caustics from refractive "
              "surfaces onto caustics receivers."),
            S("primvars:cycles:object:is_caustics_receiver", "Receive Shadow Caustics",
              "With Manifold Next Event Estimation, receive shadow caustics from caustics "
              "casters."),
        ]),
        P("Fast GI Approximation", [
            S("primvars:cycles:object:ao_distance", "AO Distance",
              "AO distance used for the Fast GI approximation (0 means use the world setting).",
              range=(0, 10, True, False)),
        ]),
    ], open=True),
    P("Shading", [
        P("Shadow Terminator", [
            S("primvars:cycles:object:shadow_terminator_geometry_offset", "Geometry Offset",
              "Offset rays from the surface to reduce shadow terminator artifact on low poly "
              "geometry. Only affects triangles at grazing angles to light.",
              range=(0, 1, True, True)),
            S("primvars:cycles:object:shadow_terminator_shading_offset", "Shading Offset",
              "Push the shadow terminator towards the light to hide artifacts on low poly "
              "geometry.",
              range=(0, 1, True, True)),
        ], open=True),
    ]),
]


def settings(items=LAYOUT):
    """All settings in the layout, depth first."""
    for item in items:
        if "panel" in item:
            yield from settings(item["items"])
        else:
            yield item


def load_sockets(json_path):
    """Map "cycles:<node>:<socket>" names to socket descriptions."""
    with open(json_path, "r") as f:
        data = json.load(f)
    sockets = dict(SESSION_SETTINGS)
    for node in data["nodes"]:
        for socket in node["inputs"]:
            sockets["cycles:%s:%s" % (node["name"], socket["name"])] = socket
    return sockets


def quote(text):
    return '"%s"' % text.replace("\\", "\\\\").replace('"', '\\"')


def format_value(value):
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, float):
        return repr(value)
    if isinstance(value, str):
        return quote(value)
    return str(value)


def encode_condition(condition, encode):
    """Encode the setting names in a disable/hide condition as parameter names."""
    tokens = []
    for token in condition.split():
        tokens.append(encode(token) if token.startswith(("cycles:", "primvars:")) else token)
    return " ".join(tokens)


def menu_items(setting, socket):
    if setting["name"] in MENU_ITEMS:
        items = MENU_ITEMS[setting["name"]]
        missing = [token for token in items if token not in socket["enum_values"]]
        if missing:
            raise RuntimeError("Unknown menu items for %s: %s" % (setting["name"], missing))
        return items
    if setting["name"] == "cycles:device" and sys.platform != "darwin":
        return {k: v for k, v in socket["enum_values"].items() if k != "METAL"}
    return socket["enum_values"]


def encode_parm(name):
    return hou.text.encodeParm(name)


class Writer:
    """Writes the layout as a dialog script.

    mode is "global" (Render Settings LOP tab, with Edit Properties controls), "viewport"
    (viewport display options) or "lop" (Cycles Render Settings LOP parameters, always authored).
    """

    def __init__(self, sockets, mode, encode=encode_parm, layout=LAYOUT):
        self.sockets = sockets
        self.mode = mode
        self.encode = encode
        self.layout = layout
        self.lines = []

    def include(self, setting):
        return self.mode != "viewport" or setting["viewport"]

    def has_settings(self, panel):
        return any(self.include(s) for s in settings(panel["items"]))

    def write_panel(self, panel, depth, parent_conditions):
        indent = "    " * depth
        name = panel["panel"]
        self.lines += [
            "",
            indent + "groupcollapsible {",
            indent + "    name    %s" % quote(
                "cycles_" + name.lower().replace(" ", "_").replace("-", "_") + "_%d" % depth),
            indent + "    label   %s" % quote(name),
        ]
        if panel["open"]:
            self.lines.append(indent + '    grouptag { "group_default" "1" }')
        # Everything in a panel with a header toggle is disabled when the toggle is off.
        conditions = list(parent_conditions)
        if panel["toggle"]:
            conditions.append("{ %s == 0 }" % panel["toggle"])
        for item in panel["items"]:
            if "panel" in item:
                if self.has_settings(item):
                    self.write_panel(item, depth + 1, conditions)
                continue
            if not self.include(item):
                continue
            is_toggle = item["name"] == panel["toggle"]
            self.write_parm(item, name, depth + 1,
                            parent_conditions if is_toggle else conditions)
        self.lines.append(indent + "}")

    def write_parm(self, setting, section, depth, disable_conditions):
        socket = self.sockets.get(setting["name"])
        if socket is None:
            raise RuntimeError("Setting %s not found in render_settings.json" % setting["name"])
        usd_type, parm_type, control_type = TYPES[socket["type"]]
        name = setting["name"]
        parm_name = self.encode(name)
        control_name = self.encode(name + "_control")
        with_control = self.mode == "global"
        indent = "    " * depth
        lines = self.lines

        if self.mode == "viewport":
            scope = "viewport toolbar" if setting["toolbar_icon"] else "viewport"
        else:
            scope = "None"

        if with_control:
            lines += [
                indent + "parm {",
                indent + "    name    %s" % quote(control_name),
                indent + "    label   %s" % quote(setting["label"]),
                indent + "    type    string",
                indent + '    default { "none" }',
                indent + "    menujoin {",
                indent + '        [ "import loputils" ]',
                indent + "        [ \"return loputils.createEditPropertiesControlMenu(kwargs, "
                "'%s')\" ]" % control_type,
                indent + "        language python",
                indent + "    }",
                indent + '    parmtag { "sidefx::look" "icon" }',
            ]
            if setting["hidewhen"]:
                lines.append(indent + "    hidewhen %s" % quote(
                    encode_condition(setting["hidewhen"], self.encode)))
            lines.append(indent + "}")

        lines += [
            indent + "parm {",
            indent + "    name    %s" % quote(parm_name),
            indent + "    label   %s" % quote(setting["label"]),
            indent + "    type    %s" % parm_type,
            indent + "    size    1",
            indent + "    default { %s }" % format_value(socket["default"]),
        ]
        if setting["range"]:
            low, high, low_strict, high_strict = setting["range"]
            lines.append(indent + "    range   { %s%s %s%s }" % (
                low, "!" if low_strict else "", high, "!" if high_strict else ""))
        if socket["type"] == "menu":
            lines.append(indent + "    menu {")
            for token, label in menu_items(setting, socket).items():
                lines.append(indent + "        %s %s" % (quote(token), quote(label)))
            lines.append(indent + "    }")
        lines.append(indent + "    help    %s" % quote(setting["help"]))
        if self.mode != "lop":
            lines.append(indent + '    parmtag { "spare_category" %s }' % quote(section))
            lines.append(indent + '    parmtag { "uiscope" %s }' % quote(scope))
            lines.append(indent + '    parmtag { "usdvaluetype" %s }' % quote(usd_type))
        if setting["toolbar_icon"] and scope != "None":
            lines.append(indent + '    parmtag { "uiicon" %s }' % setting["toolbar_icon"])
        disable = []
        if with_control:
            disable.append("{ %s == block } { %s == none }" % (control_name, control_name))
        disable += disable_conditions
        if setting["disablewhen"]:
            disable.append(setting["disablewhen"])
        if disable:
            lines.append(indent + "    disablewhen %s" % quote(
                encode_condition(" ".join(disable), self.encode)))
        if setting["hidewhen"]:
            lines.append(indent + "    hidewhen %s" % quote(
                encode_condition(setting["hidewhen"], self.encode)))
        lines.append(indent + "}")

    def generate(self):
        lines = self.lines
        if self.mode == "lop":
            lines += ["{", '    name    "cycles"', '    label   "Cycles"']
        else:
            lines += [
                "/// Automatically generated by rendersettings2ds.py, do not edit.",
                "/// %s for the Cycles render delegate." % (
                    "Viewport display options" if self.mode == "viewport" else
                    "Object settings" if self.layout is GEOMETRY_LAYOUT else "Render settings"),
                "",
                '#include "$HFS/houdini/soho/parameters/CommonMacros.ds"',
                "",
                "{",
                '    name    "cycles"',
                '    label   "Cycles"',
                '    parmtag { spare_opfilter "!!SHOP/PROPERTIES!!" }',
                '    parmtag { spare_classtags "render" }',
            ]
        for panel in self.layout:
            if self.has_settings(panel):
                self.write_panel(panel, 1, [])
        lines += ["}", ""]
        return "\n".join(lines)


def generate(sockets, viewport):
    return Writer(sockets, "viewport" if viewport else "global").generate()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--json", required=True, help="Path to render_settings.json")
    parser.add_argument("--ds-install-dir", required=True,
                        help="Directory to write the dialog scripts to (soho/parameters)")
    args = parser.parse_args()

    sockets = load_sockets(args.json)
    os.makedirs(args.ds_install_dir, exist_ok=True)

    for suffix, ds in (("Global", generate(sockets, False)),
                       ("Viewport", generate(sockets, True)),
                       ("Geometry", Writer(GEOMETRY_SETTINGS, "global",
                                           layout=GEOMETRY_LAYOUT).generate())):

        # Validate by parsing the dialog script like Houdini does.
        group = hou.ParmTemplateGroup()
        group.setToDialogScript(ds)

        path = os.path.join(args.ds_install_dir, "HdCyclesPlugin_%s.ds" % suffix)
        with open(path, "w", newline="\n") as f:
            f.write(ds)
        log("Wrote %s" % path)

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        log("Error: %s" % e)
        sys.exit(1)
