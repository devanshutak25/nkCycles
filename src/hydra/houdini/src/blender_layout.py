"""
Match Cycles shader nodes to the layout of Blender's shader nodes.

blender_node_layouts.json (written by blender_node_layouts.py inside Blender) describes how Blender
draws every shader node. This module maps that onto the Cycles node sockets from shader_nodes.json
and returns, per Cycles node, the parameters in Blender's order with Blender's labels, menus,
defaults, ranges, panels and visibility conditions. Cycles sockets that Blender does not show are
appended after them.
"""

import math
import re

# Blender node for Cycles nodes whose labels differ.
BLENDER_NODES = {
    "add_closure": "ShaderNodeAddShader",
    "background_shader": "ShaderNodeBackground",
    "camera_info": "ShaderNodeCameraData",
    "hair_info": "ShaderNodeHairInfo",
    "hsv": "ShaderNodeHueSaturation",
    "invert": "ShaderNodeInvert",
    "mix": "ShaderNodeMixRGB",
    "mix_closure": "ShaderNodeMixShader",
    "ies_light": "ShaderNodeTexIES",
    "rgb_ramp": "ShaderNodeValToRGB",
    "vertex_color": "ShaderNodeVertexColor",
    "absorption_volume": "ShaderNodeVolumeAbsorption",
    "scatter_volume": "ShaderNodeVolumeScatter",
}

# Cycles socket for Blender properties and inputs (by identifier) whose names differ.
ALIASES = {
    "*": {"uv_map": "attribute", "from_instancer": "from_dupli"},
    "attribute": {"attribute_name": "attribute"},
    "add_closure": {"Shader": "closure1", "Shader_001": "closure2"},
    "mix_closure": {"Shader": "closure1", "Shader_001": "closure2"},
    "combine_color": {"mode": "color_type"},
    "separate_color": {"mode": "color_type"},
    "map_range": {"interpolation_type": "range_type"},
    "vector_map_range": {"interpolation_type": "range_type", "clamp": "use_clamp"},
    "math": {"operation": "math_type", "Value": "value1", "Value_001": "value2",
             "Value_002": "value3"},
    "vector_math": {"operation": "math_type", "Vector": "vector1", "Vector_001": "vector2",
                    "Vector_002": "vector3"},
    "mix": {"blend_type": "mix_type"},
    "mix_float": {"clamp_factor": "use_clamp"},
    "mix_vector": {"clamp_factor": "use_clamp"},
    "mix_vector_non_uniform": {"clamp_factor": "use_clamp"},
    "mix_color": {"clamp_factor": "use_clamp", "clamp_result": "use_clamp_result"},
    "wave_texture": {"wave_profile": "profile"},
    "ies_light": {"filepath": "filename"},
}

# Cycles menu values for Blender menu items whose names differ.
MENU_ALIASES = {
    "extension": {"REPEAT": "periodic", "EXTEND": "clamp", "CLIP": "black"},
    "rotate_type": {"AXIS_ANGLE": "axis"},
}

# Blender properties that have no Cycles socket on purpose (fixed by the node variant, or only
# used by Blender itself).
IGNORED_PROPERTIES = {"factor_mode", "use_alpha", "attribute_type"}

# Cycles sockets for settings Blender shows as a data-block selector (image, IES file) at the top.
LEADING_SOCKETS = {
    "image_texture": [("filename", "Image"), ("colorspace", "Color Space"),
                      ("alpha_type", "Alpha")],
    "environment_texture": [("filename", "Image"), ("colorspace", "Color Space"),
                            ("alpha_type", "Alpha")],
    "ies_light": [("filename", "IES File")],
    "value": [("value", "Value")],
    "color": [("value", "Color")],
}

# Cycles sockets that Blender sets internally and that are not useful to edit.
INTERNAL_SOCKETS = {
    "*": {"tiles", "animated"},
    "bump": {"use_object_space", "sample_center", "sample_x", "sample_y"},
    "texture_coordinate": {"use_transform", "ob_tfm"},
    "ies_light": {"ies"},
}

TEXTURE_MAPPING_PANEL = "Texture Mapping"
TEXTURE_MAPPING_LABELS = {
    "translation": "Location", "rotation": "Rotation", "scale": "Scale", "min": "Minimum",
    "max": "Maximum", "use_minmax": "Clamp", "x_mapping": "X", "y_mapping": "Y",
    "z_mapping": "Z", "type": "Vector Type", "projection": "Projection",
}

# Largest float that is still shown as a range limit (Blender uses +-FLT_MAX for no limit).
MAX_RANGE = 1.0e6


def normalize(name):
    return re.sub(r"[^a-z0-9]", "", name.lower())


def title(name):
    """Label for Cycles sockets and menu items that Blender does not show."""
    name = name.replace(".", " ").replace("_", " ")
    words = []
    for word in name.split():
        upper = word.upper()
        if upper in ("IOR", "BSDF", "BSSRDF", "GGX", "RGB", "UV", "AO", "HSV", "XYZ", "IES",
                     "BW", "OSL"):
            words.append(upper)
        else:
            words.append(word[:1].upper() + word[1:])
    return " ".join(words)


def find_blender_node(cycles_node, layouts):
    name = cycles_node["name"]
    if name in layouts["variants"]:
        return layouts["variants"][name]
    idname = BLENDER_NODES.get(name)
    if idname:
        return layouts["nodes"].get(idname)
    wanted = normalize(cycles_node.get("label", name))
    for layout in layouts["nodes"].values():
        if normalize(layout["label"]) == wanted:
            return layout
    return None


# Blender puts the variants of one node in different Add menus.
VARIANT_MENUS = {
    "mix_float": "Utilities/Math",
    "map_range": "Utilities/Math",
    "mix_color": "Color",
    "mix_vector": "Utilities/Vector",
    "mix_vector_non_uniform": "Utilities/Vector",
    "vector_map_range": "Utilities/Vector",
}


def tab_menu(cycles_node, layouts):
    """Tab menu path of a Cycles node, following Blender's Add menu ("Cycles/Utilities/Math")."""
    path = VARIANT_MENUS.get(cycles_node["name"])
    if not path and layouts:
        layout = find_blender_node(cycles_node, layouts)
        path = layouts.get("menus", {}).get(layout["blender_idname"]) if layout else None
    return "Cycles/" + path if path else "Cycles"


def find_socket(cycles_node, item, sockets):
    """Cycles socket for a Blender layout item, or None."""
    aliases = dict(ALIASES["*"])
    aliases.update(ALIASES.get(cycles_node["name"], {}))
    key = item["identifier"]
    if key in aliases:
        return sockets.get(aliases[key])
    # Texture mapping sockets have generic labels (Scale, Type), only match them by name.
    own = [s for s in sockets.values() if not s["name"].startswith("tex_mapping.")]
    for candidate in (normalize(key), normalize(item["label"])):
        for socket in own:
            if normalize(socket["name"]) == candidate:
                return socket
        for socket in own:
            if normalize(socket.get("ui_name", "")) == candidate:
                return socket
    return None


def map_menu(socket, item, problems):
    """Menu entries (Cycles token, label) in Blender's order, and the token map."""
    tokens = socket.get("enum_values") or {}
    by_norm = {}
    for token, label in tokens.items():
        by_norm.setdefault(normalize(token), token)
        by_norm.setdefault(normalize(label), token)
    entries = []
    token_of = {}
    aliases = MENU_ALIASES.get(socket["name"], {})
    for identifier, label, _description in item.get("items", []):
        token = aliases.get(identifier) or by_norm.get(normalize(identifier)) or             by_norm.get(normalize(label))
        if token is None or token in token_of.values():
            problems.append("menu item %s.%s has no Cycles value" % (socket["name"], identifier))
            continue
        token_of[identifier] = token
        entries.append((token, label))
    # Cycles values that Blender does not offer are kept, so existing scenes still load.
    for token, label in tokens.items():
        if token not in token_of.values():
            entries.append((token, title(label)))
    return entries, token_of


def convert_default(socket, value, token_of):
    """Blender default value as a value for the Cycles socket, or None."""
    socket_type = socket["type"]
    if value is None:
        return None
    if socket_type == "menu":
        return token_of.get(value)
    if socket_type == "boolean":
        return bool(value)
    if socket_type in ("int", "uint"):
        return int(value)
    if socket_type == "float":
        if isinstance(value, list):
            return None
        return float(value)
    if socket_type in ("color", "vector", "point", "normal"):
        if not isinstance(value, list) or len(value) < 3:
            return None
        return [float(x) for x in value[:3]]
    if socket_type == "point2":
        return [float(x) for x in value[:2]] if isinstance(value, list) else None
    if socket_type == "string":
        return str(value)
    return None


# Blender's declared limits at or beyond this mean "no limit" (-1000 to 1000 for texture
# coordinates, -10000 to 10000 for generic values).
NO_LIMIT = 1000.0
# A range is too wide for a Houdini slider when it spans this many times the default value.
WIDE_RANGE = 50.0
# Slider end for wide IOR ranges (IOR 1 to 1000 in Blender).
IOR_SLIDER_MAX = 3.0

# Slider ranges (min, max, min_strict, max_strict) where Blender's range does not fit: angles
# that Blender wraps instead of limiting, AOV values that are often outside 0 to 1, and thin film
# thicknesses in nanometers.
RANGE_OVERRIDES = {
    "*": {"thin_film_thickness": (0.0, 1000.0, True, False)},
    "aov_output": {"value": (0.0, 1.0, False, False)},
    "sky_texture": {"sun_elevation": (-math.pi / 2, math.pi / 2, False, False),
                    "sun_rotation": (0.0, 2.0 * math.pi, False, False)},
    "vector_rotate": {"angle": (-math.pi, math.pi, False, False)},
    "gabor_texture": {"orientation_2d": (0.0, math.pi, False, False)},
}


def ceil_power_of_ten(value):
    return 10.0 ** math.ceil(math.log10(value) - 1e-6)


def ui_range(cycles_node, item, socket):
    """(min, max, min_strict, max_strict) for the parameter slider, or None.

    Sockets use the min and max their node declares, node properties their soft range. Houdini
    has one range for both the slider and the clamp, while Blender drags without a slider, so
    ranges far wider than the default value (IOR 1 to 1000, Strength 0 to 1000000) get a
    practical slider that can be typed beyond, and only limits that are real ones (Roughness 0
    to 1, IOR >= 1) are strict.
    """
    override = RANGE_OVERRIDES.get(cycles_node, {}).get(socket["name"]) or         RANGE_OVERRIDES["*"].get(socket["name"])
    if override:
        return override
    soft = item.get("soft_range")
    hard = item.get("hard_range")
    if not soft or socket["type"] not in ("float", "int", "uint"):
        return None
    low, high = soft
    if not (math.isfinite(low) and math.isfinite(high)) or abs(low) > MAX_RANGE or \
       abs(high) > MAX_RANGE:
        return None
    if item.get("kind") == "property":
        low_strict = bool(hard) and hard[0] == low
        high_strict = bool(hard) and hard[1] == high
    else:
        low_strict = abs(low) < NO_LIMIT
        high_strict = abs(high) < NO_LIMIT

    default = item.get("default")
    if not isinstance(default, (int, float)) or isinstance(default, bool):
        default = 0.0
    scale = max(abs(default), 1.0)
    if high - low > WIDE_RANGE * scale:
        if "IOR" in item.get("label", "") and default >= 1.0:
            wide_high = IOR_SLIDER_MAX
        else:
            wide_high = max(ceil_power_of_ten(2.0 * abs(default)), 1.0) if default else 1.0
        if low < 0.0:
            # Signed values centered on zero keep a symmetric slider, others start at zero.
            wide_low = -wide_high if default == 0.0 and -low >= wide_high else 0.0
            if default < 0.0:
                wide_low = max(low, -wide_high)
            low, low_strict = wide_low, False
        if wide_high < high:
            high, high_strict = wide_high, False
    if socket["type"] != "float":
        low, high = int(math.floor(low)), int(math.ceil(high))
    return (low, high, low_strict, high_strict)


def same_value(a, b):
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(abs(x - y) < 1e-6 for x, y in zip(a, b))
    if isinstance(a, float) or isinstance(b, float):
        try:
            return abs(float(a) - float(b)) < 1e-6
        except (TypeError, ValueError):
            return False
    return a == b


def node_layout(cycles_node, layouts, problems):
    """Parameters of a Cycles node in Blender's layout.

    Returns (label, panels, params). Each param is a dict with the Cycles "socket", "label",
    "default", "menu" (list of (token, label)), "range", "panel", "hidden" (always hidden),
    "hidewhen" (list of {socket name: token} conditions any of which hides it) and "help".
    """
    name = cycles_node["name"]
    sockets = {s["name"]: s for s in cycles_node["inputs"]}
    blender = find_blender_node(cycles_node, layouts)
    internal = INTERNAL_SOCKETS["*"] | INTERNAL_SOCKETS.get(name, set())

    params = []
    used = set()

    def add(socket, **kwargs):
        param = {"socket": socket, "label": title(socket.get("ui_name") or socket["name"]),
                 "default": socket.get("default"), "menu": None, "range": None, "panel": None,
                 "hidden": False, "hidewhen": [], "help": ""}
        if socket["type"] == "menu":
            param["menu"] = [(t, title(l)) for t, l in (socket.get("enum_values") or {}).items()]
        param.update(kwargs)
        params.append(param)
        used.add(socket["name"])
        return param

    for socket_name, label in LEADING_SOCKETS.get(name, []):
        if socket_name in sockets:
            add(sockets[socket_name], label=label)

    if blender is None:
        label = title(cycles_node.get("label", name))
        panels = []
    else:
        label = blender["label"]
        panels = list(blender["panels"])
        token_maps = {}
        mapped = []
        for item in blender["items"]:
            if item["identifier"] == "__extend__":
                continue  # Virtual socket for connecting new inputs in Blender.
            socket = find_socket(cycles_node, item, sockets)
            if socket is None or socket["name"] in used:
                ignored = item["identifier"] in IGNORED_PROPERTIES or (
                    name.startswith("mix_") and item["identifier"] in ("blend_type",
                                                                       "clamp_result"))
                if not ignored and (item["kind"] == "property" or not item.get("hide_value")):
                    problems.append("%s: Blender %s %s has no Cycles socket" % (
                        name, item["kind"], item["identifier"]))
                continue
            menu = None
            token_of = {}
            if socket["type"] == "menu":
                menu, token_of = map_menu(socket, item, problems)
                token_maps[item["identifier"]] = (socket["name"], token_of)
            default = convert_default(socket, item.get("default"), token_of)
            if default is None:
                default = socket.get("default")
            param = add(socket, label=item["label"], default=default, menu=menu,
                        range=ui_range(name, item, socket), panel=item.get("panel"),
                        help=item.get("description", ""))
            mapped.append((param, item))

        # Visibility conditions, translated to Cycles socket names and menu tokens.
        for param, item in mapped:
            conditions = []
            for condition in item.get("hidden_when", []):
                translated = {}
                for prop, value in condition.items():
                    if prop not in token_maps:
                        translated = None
                        break
                    socket_name, token_of = token_maps[prop]
                    if value not in token_of:
                        translated = None
                        break
                    translated[socket_name] = token_of[value]
                if translated:
                    conditions.append(translated)
            param["hidewhen"] = conditions

    # Remaining Cycles sockets.
    for socket in cycles_node["inputs"]:
        if socket["name"] in used:
            continue
        if socket["name"].startswith("tex_mapping."):
            key = socket["name"][len("tex_mapping."):]
            add(socket, label=TEXTURE_MAPPING_LABELS.get(key, title(key)),
                panel=TEXTURE_MAPPING_PANEL)
            if TEXTURE_MAPPING_PANEL not in panels:
                panels.append(TEXTURE_MAPPING_PANEL)
        else:
            add(socket, hidden=socket["name"] in internal)

    # Panel parameters go after the others, in panel order.
    top = [p for p in params if not p["panel"]]
    in_panels = [p for panel in panels for p in params if p["panel"] == panel]
    return label, panels, top + in_panels


def changed_defaults(cycles_node, params):
    """Parameters whose (Blender) default differs from the Cycles socket default."""
    result = {}
    for param in params:
        socket = param["socket"]
        if param["hidden"] or socket["type"] in ("closure", "node", "matrix"):
            continue
        if param["default"] is None or same_value(param["default"], socket.get("default")):
            continue
        result[socket["name"]] = param["default"]
    return result
