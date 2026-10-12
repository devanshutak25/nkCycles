# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import nuke

nuke.menu("Nodes").addCommand("3D/CyclesRender", lambda: nuke.createNode("CyclesRender"))
