# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Game clients of the Angelica Engine (Perfect World) inside the Studio.

- ``clients``: the client folders the user added (kept in the Studio's config folder).
- ``library``: the files of a client grouped by container (each ``.pck`` package, each loose
  folder such as ``maps``), browsable as a tree and searchable.
- ``sources``: opening client files as Studio data and remembering where they came from.

File access goes through the native ``_angelica`` module (packages are read in place, nothing is
extracted to disk).
"""
