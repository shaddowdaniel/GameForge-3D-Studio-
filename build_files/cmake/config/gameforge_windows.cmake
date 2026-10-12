# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Release build of GameForge 3D Studio for Windows, as made by
# `.github/workflows/windows-installer.yml`:
#
#   cmake -C build_files/cmake/config/gameforge_windows.cmake -G Ninja -S . -B <build-dir>
#
# The Blender release configuration without the GPU kernels that need the CUDA, HIP and
# oneAPI toolkits and the OptiX SDK (not on the GitHub runners). Cycles renders on the CPU,
# and on CUDA when the CUDA toolkit is installed to compile its kernels at first use.

include("${CMAKE_CURRENT_LIST_DIR}/blender_release.cmake")

set(WITH_CYCLES_CUDA_BINARIES   OFF CACHE BOOL "" FORCE)
set(WITH_CYCLES_HIP_BINARIES    OFF CACHE BOOL "" FORCE)
set(WITH_CYCLES_DEVICE_HIPRT    OFF CACHE BOOL "" FORCE)
set(WITH_CYCLES_ONEAPI_BINARIES OFF CACHE BOOL "" FORCE)
set(WITH_CYCLES_DEVICE_ONEAPI   OFF CACHE BOOL "" FORCE)
set(WITH_CYCLES_DEVICE_OPTIX    OFF CACHE BOOL "" FORCE)

set(WITH_IO_ANGELICA            ON  CACHE BOOL "" FORCE)
