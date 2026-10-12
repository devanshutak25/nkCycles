#!/usr/bin/env hython
"""
Generate the Houdini render settings UI for Cycles from render_settings.json.

Usage:
    hython rendersettings2ds.py --json <render_settings.json> --ds-install-dir <output_dir>

Houdini looks for dialog scripts named after the render delegate plugin in
$HOUDINI_PATH/soho/parameters:

  HdCyclesPlugin_Global.ds    Cycles tab of the Render Settings LOP.
  HdCyclesPlugin_Viewport.ds  Cycles section of the viewport display options.

Each parameter is a "cycles:..." render setting forwarded to the delegate.
Types, defaults and menus come from the Cycles node sockets dumped to
render_settings.json, so the UI always shows what the delegate uses when a
setting is not authored. Labels and layout follow Blender's render properties.
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


def S(name, label, help, range=None, viewport=False, toolbar_icon=None, hidewhen=None):
    """A single setting in the layout."""
    return {
        "name": name,
        "label": label,
        "help": help,
        "range": range,
        "viewport": viewport,
        "toolbar_icon": toolbar_icon,
        "hidewhen": hidewhen,
    }


# Layout of the settings, as (section, [settings]).
LAYOUT = [
    ("Device", [
        S("cycles:device", "Device",
          "Device to render with. Automatic uses the best available GPU (OptiX, then CUDA, "
          "HIP, oneAPI, Metal) and falls back to the CPU. Changing it restarts the render.",
          viewport=True),
        S("cycles:threads", "Threads",
          "Number of CPU threads to use. 0 uses all cores.",
          range=(0, 64, True, False)),
    ]),
    ("Sampling", [
        S("cycles:samples", "Max Samples",
          "Number of samples to render for each pixel.",
          range=(1, 4096, True, False), viewport=True),
        S("cycles:time_limit", "Time Limit",
          "Limit the render time in seconds, excluding synchronization time. 0 disables the limit.",
          range=(0, 600, True, False)),
        S("cycles:integrator:use_adaptive_sampling", "Adaptive Sampling",
          "Automatically reduce the number of samples per pixel based on estimated noise level.",
          viewport=True),
        S("cycles:integrator:adaptive_threshold", "Noise Threshold",
          "Noise level step to stop sampling at, lower values reduce noise at the cost of "
          "render time. 0 for automatic setting based on the number of samples.",
          range=(0, 1, True, True), viewport=True),
        S("cycles:integrator:adaptive_min_samples", "Min Samples",
          "Minimum number of samples before adaptive sampling stops a pixel. 0 for automatic "
          "setting based on the noise threshold.",
          range=(0, 4096, True, False)),
    ]),
    ("Denoising", [
        S("cycles:integrator:use_denoise", "Denoise",
          "Denoise the rendered image.",
          viewport=True, toolbar_icon="VIEW_display_denoise"),
        S("cycles:integrator:denoiser_type", "Denoiser",
          "Denoiser to use. OptiX requires an NVIDIA GPU.",
          viewport=True),
        S("cycles:integrator:denoiser_prefilter", "Prefilter",
          "Prefilter of the guiding passes (albedo and normal). Accurate gives the best detail, "
          "None should only be used when the guiding passes are noise free."),
        S("cycles:integrator:denoiser_quality", "Quality",
          "Quality of the OpenImageDenoise denoiser, balanced and fast trade quality for speed."),
        S("cycles:integrator:denoise_use_gpu", "Use GPU",
          "Run the denoiser on the GPU when available."),
        S("cycles:integrator:denoise_start_sample", "Start Sample",
          "Sample to start denoising the interactive render at.",
          range=(0, 64, True, False), viewport=True),
    ]),
    ("Advanced Sampling", [
        S("cycles:integrator:seed", "Seed",
          "Seed value for the random number generator, to get different noise patterns.",
          range=(0, 100, True, False)),
        S("cycles:sample_offset", "Sample Offset",
          "Number of samples to skip when starting the render.",
          range=(0, 4096, True, False)),
        S("cycles:integrator:sampling_pattern", "Pattern",
          "Random sampling pattern used by the integrator."),
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
          "first bounces, but can also be less efficient for more complex geometry like hair.",
          range=(0, 32, True, False)),
        S("cycles:integrator:use_light_tree", "Light Tree",
          "Sample multiple lights more efficiently based on estimated contribution at every "
          "shading point."),
        S("cycles:integrator:light_sampling_threshold", "Light Threshold",
          "Probabilistically terminate light samples when the light contribution is below "
          "this threshold (more noise but faster rendering). 0 disables the test.",
          range=(0, 1, True, False)),
    ]),
    ("Path Guiding", [
        S("cycles:integrator:use_guiding", "Path Guiding",
          "Use path guiding for sampling paths. Path guiding incrementally learns the light "
          "distribution of the scene and guides paths into directions with high direct and "
          "indirect light contributions. Only supported on the CPU."),
        S("cycles:integrator:guiding_training_samples", "Training Samples",
          "The maximum number of samples used for training path guiding. 0 trains for all samples.",
          range=(0, 1024, True, False)),
        S("cycles:integrator:use_surface_guiding", "Surface Guiding",
          "Use guiding when sampling directions on a surface."),
        S("cycles:integrator:use_volume_guiding", "Volume Guiding",
          "Use guiding when sampling directions inside a volume."),
    ]),
    ("Light Paths", [
        S("cycles:integrator:max_bounce", "Total Bounces",
          "Total maximum number of bounces.",
          range=(0, 32, True, False), viewport=True),
        S("cycles:integrator:max_diffuse_bounce", "Diffuse Bounces",
          "Maximum number of diffuse reflection bounces, bounded by total maximum.",
          range=(0, 32, True, False), viewport=True),
        S("cycles:integrator:max_glossy_bounce", "Glossy Bounces",
          "Maximum number of glossy reflection bounces, bounded by total maximum.",
          range=(0, 32, True, False), viewport=True),
        S("cycles:integrator:max_transmission_bounce", "Transmission Bounces",
          "Maximum number of transmission bounces, bounded by total maximum.",
          range=(0, 32, True, False), viewport=True),
        S("cycles:integrator:max_volume_bounce", "Volume Bounces",
          "Maximum number of volumetric scattering events.",
          range=(0, 32, True, False), viewport=True),
        S("cycles:integrator:transparent_max_bounce", "Transparent Bounces",
          "Maximum number of transparent bounces. This is independent of maximum number of "
          "other bounces.",
          range=(0, 32, True, False), viewport=True),
    ]),
    ("Clamping", [
        S("cycles:integrator:sample_clamp_direct", "Direct Light",
          "If non-zero, the maximum value for a direct sample, higher values will be scaled "
          "down to avoid too much noise and slow convergence at the cost of accuracy.",
          range=(0, 100, True, False)),
        S("cycles:integrator:sample_clamp_indirect", "Indirect Light",
          "If non-zero, the maximum value for an indirect sample, higher values will be scaled "
          "down to avoid too much noise and slow convergence at the cost of accuracy.",
          range=(0, 100, True, False)),
    ]),
    ("Caustics", [
        S("cycles:integrator:filter_glossy", "Filter Glossy",
          "Adaptively blur glossy shaders after blurry bounces, to reduce noise at the cost "
          "of accuracy.",
          range=(0, 10, True, False)),
        S("cycles:integrator:caustics_reflective", "Reflective Caustics",
          "Use reflective caustics, resulting in a brighter image (more noise but added realism)."),
        S("cycles:integrator:caustics_refractive", "Refractive Caustics",
          "Use refractive caustics, resulting in a brighter image (more noise but added realism)."),
    ]),
    ("Volumes", [
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
    ("Film", [
        S("cycles:film:exposure", "Exposure",
          "Image brightness scale.",
          range=(0, 10, True, False)),
        S("cycles:film:filter_type", "Pixel Filter",
          "Pixel filter type."),
        S("cycles:film:filter_width", "Filter Width",
          "Pixel filter width.",
          range=(0.01, 10, True, False)),
        S("cycles:background:transparent", "Transparent",
          "World background is transparent, for compositing the render over another "
          "background. When not set, the background is transparent unless the scene has a "
          "dome light."),
        S("cycles:background:transparent_glass", "Transparent Glass",
          "Render transmissive surfaces as transparent, for compositing glass over another "
          "background."),
        S("cycles:background:transparent_roughness_threshold", "Roughness Threshold",
          "For transparent transmission, keep surfaces with roughness above the threshold opaque.",
          range=(0, 1, True, True)),
    ]),
]

# USD value type, Houdini parameter type and Edit Properties control type per socket type.
TYPES = {
    "boolean": ("bool", "toggle", "bool"),
    "int": ("int", "int", "int"),
    "float": ("float", "float", "float"),
    "menu": ("token", "string", "string"),
}


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


def encode_condition(condition):
    """Encode the setting names in a disable/hide condition as parameter names."""
    tokens = []
    for token in condition.split():
        tokens.append(hou.text.encodeParm(token) if token.startswith("cycles:") else token)
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


def write_parm(lines, setting, socket, section, scope, with_control):
    usd_type, parm_type, control_type = TYPES[socket["type"]]
    name = setting["name"]
    parm_name = hou.text.encodeParm(name)
    control_name = hou.text.encodeParm(name + "_control")

    if with_control:
        lines += [
            "        parm {",
            "            name    %s" % quote(control_name),
            "            label   %s" % quote(setting["label"]),
            "            type    string",
            '            default { "none" }',
            "            menujoin {",
            '                [ "import loputils" ]',
            "                [ \"return loputils.createEditPropertiesControlMenu(kwargs, '%s')\" ]"
            % control_type,
            "                language python",
            "            }",
            '            parmtag { "sidefx::look" "icon" }',
        ]
        if setting["hidewhen"]:
            lines.append("            hidewhen %s" % quote(encode_condition(setting["hidewhen"])))
        lines.append("        }")

    lines += [
        "        parm {",
        "            name    %s" % quote(parm_name),
        "            label   %s" % quote(setting["label"]),
        "            type    %s" % parm_type,
        "            size    1",
        "            default { %s }" % format_value(socket["default"]),
    ]
    if setting["range"]:
        low, high, low_strict, high_strict = setting["range"]
        lines.append("            range   { %s%s %s%s }" % (
            low, "!" if low_strict else "", high, "!" if high_strict else ""))
    if socket["type"] == "menu":
        lines.append("            menu {")
        for token, label in menu_items(setting, socket).items():
            lines.append("                %s %s" % (quote(token), quote(label)))
        lines.append("            }")
    lines.append("            help    %s" % quote(setting["help"]))
    lines.append('            parmtag { "spare_category" %s }' % quote(section))
    lines.append('            parmtag { "uiscope" %s }' % quote(scope))
    lines.append('            parmtag { "usdvaluetype" %s }' % quote(usd_type))
    if setting["toolbar_icon"] and scope != "None":
        lines.append('            parmtag { "uiicon" %s }' % setting["toolbar_icon"])
    if with_control:
        lines.append("            disablewhen %s" % quote(
            "{ %s == block } { %s == none }" % (control_name, control_name)))
    if setting["hidewhen"]:
        lines.append("            hidewhen %s" % quote(encode_condition(setting["hidewhen"])))
    lines.append("        }")


def generate(sockets, viewport):
    lines = [
        "/// Automatically generated by rendersettings2ds.py, do not edit.",
        "/// %s for the Cycles render delegate." % (
            "Viewport display options" if viewport else "Render settings"),
        "",
        '#include "$HFS/houdini/soho/parameters/CommonMacros.ds"',
        "",
        "{",
        '    name    "cycles"',
        '    label   "Cycles"',
        '    parmtag { spare_opfilter "!!SHOP/PROPERTIES!!" }',
        '    parmtag { spare_classtags "render" }',
    ]

    for section, settings in LAYOUT:
        if viewport:
            settings = [setting for setting in settings if setting["viewport"]]
        if not settings:
            continue

        lines += [
            "",
            "    groupcollapsible {",
            "        name    %s" % quote("cycles_" + section.lower().replace(" ", "_")),
            "        label   %s" % quote(section),
            '        grouptag { "group_default" "1" }',
        ]
        for setting in settings:
            socket = sockets.get(setting["name"])
            if socket is None:
                raise RuntimeError("Setting %s not found in render_settings.json" %
                                   setting["name"])
            if viewport:
                scope = "viewport toolbar" if setting["toolbar_icon"] else "viewport"
            else:
                scope = "None"
            write_parm(lines, setting, socket, section, scope, with_control=not viewport)
        lines.append("    }")

    lines += ["}", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--json", required=True, help="Path to render_settings.json")
    parser.add_argument("--ds-install-dir", required=True,
                        help="Directory to write the dialog scripts to (soho/parameters)")
    args = parser.parse_args()

    sockets = load_sockets(args.json)
    os.makedirs(args.ds_install_dir, exist_ok=True)

    for suffix, viewport in (("Global", False), ("Viewport", True)):
        ds = generate(sockets, viewport)

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
