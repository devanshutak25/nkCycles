# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

"""
Prepare USD headers matching the USD libraries of a Nuke release.

Nuke ships its USD libraries (fnusd, fnhd, ...) without headers. This takes a checkout of the
OpenUSD release Nuke was built with, generates pxr/pxr.h and applies the ABI changes Foundry made
to their build, so classes subclassed by Cycles have the same virtual function tables.

Usage: prepare_usd_headers.py <nuke major version> <OpenUSD source checkout>
  Nuke 15: OpenUSD v23.05
"""

import sys
from pathlib import Path

USD_VERSIONS = {15: (23, 5)}

# Virtual functions Foundry added: (header, declaration to insert after, declaration).
# Found by comparing the vtables of Nuke's DLLs (and Storm's overrides) with the stock headers.
ABI_PATCHES = {
    15: [
        ("pxr/imaging/hd/renderDelegate.h",
         "    virtual VtDictionary GetRenderStats() const;\n",
         "\n"
         "    /// Foundry addition in Nuke's USD: whether HdxPickTask is supported.\n"
         "    HD_API\n"
         "    virtual bool SupportsPickTask() const;\n"),
    ],
}


def main() -> int:
    nuke_major = int(sys.argv[1])
    root = Path(sys.argv[2])
    major, minor = USD_VERSIONS[nuke_major]

    template = (root / "pxr" / "pxr.h.in").read_text()
    values = {
        "PXR_MAJOR_VERSION": "0",
        "PXR_MINOR_VERSION": str(major),
        "PXR_PATCH_VERSION": str(minor),
        "PXR_VERSION": f"{major:02d}{minor:02d}",
        "PXR_USE_NAMESPACES": "1",
        "PXR_EXTERNAL_NAMESPACE": "pxr",
        "PXR_INTERNAL_NAMESPACE": f"pxrInternal_v0_{major}",
        "PXR_PYTHON_SUPPORT_ENABLED": "1",
        "PXR_PREFER_SAFETY_OVER_SPEED": "1",
    }
    for key, value in values.items():
        template = template.replace(f"@{key}@", value)
    assert "@" not in template, "unhandled pxr.h.in variable"
    (root / "pxr" / "pxr.h").write_text(template)

    for header, anchor, declaration in ABI_PATCHES.get(nuke_major, []):
        path = root / header
        text = path.read_text()
        if declaration in text:
            continue
        if text.count(anchor) != 1:
            print(f"prepare_usd_headers: anchor not found in {header}", file=sys.stderr)
            return 1
        path.write_text(text.replace(anchor, anchor + declaration))
    return 0


if __name__ == "__main__":
    sys.exit(main())
