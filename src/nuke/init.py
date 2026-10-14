# SPDX-FileCopyrightText: 2026 Devanshu Tak
#
# SPDX-License-Identifier: Apache-2.0

# Cycles for Nuke. Add this folder to NUKE_PATH (or nuke.pluginAddPath it from ~/.nuke/init.py).

import os

# Libraries next to CyclesRender are loaded through PATH, Nuke loads plugins with the
# default DLL search order.
_root = os.path.dirname(os.path.abspath(__file__))
os.environ["PATH"] = _root + os.pathsep + os.environ.get("PATH", "")
