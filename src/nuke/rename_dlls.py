# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

"""
Give bundled DLLs a private name, so Windows doesn't bind them to the different version of the
same DLL that Nuke has already loaded (Windows reuses a loaded module with the same name).

The extension is changed (OpenEXR.dll -> OpenEXR.nkc), which keeps the name length so the import
tables of all DLLs in the folder can be patched in place.

Usage: rename_dlls.py <folder> <name.dll>...
"""

import re
import sys
from pathlib import Path

NEW_EXTENSION = ".nkc"


def private_name(name: str) -> str:
    return name[:-len(".dll")] + NEW_EXTENSION


def main() -> int:
    folder = Path(sys.argv[1])
    names = sys.argv[2:]
    assert all(name.lower().endswith(".dll") for name in names)

    # Import names are NUL terminated ASCII, case as written by the linker.
    patterns = [(re.compile(re.escape(name.encode("ascii")) + b"\0", re.IGNORECASE),
                 private_name(name).encode("ascii") + b"\0") for name in names]

    for path in list(folder.glob("*.dll")) + list(folder.glob("*" + NEW_EXTENSION)):
        data = path.read_bytes()
        patched = data
        for pattern, replacement in patterns:
            patched = pattern.sub(replacement, patched)
        if patched != data:
            path.write_bytes(patched)

    for name in names:
        src = folder / name
        if src.exists():
            dst = folder / private_name(name)
            dst.unlink(missing_ok=True)
            src.rename(dst)
        elif not (folder / private_name(name)).exists():
            print(f"rename_dlls: {name} not found in {folder}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
