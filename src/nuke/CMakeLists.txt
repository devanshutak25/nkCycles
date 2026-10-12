# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

#####################################################################
# Nuke render node
#####################################################################

find_package(Nuke REQUIRED CONFIG PATHS ${NUKE_ROOT}/cmake NO_DEFAULT_PATH)

add_nuke_plugin(CyclesRender
  nuke_node.cpp
  render.cpp
  render.h
)

target_include_directories(CyclesRender PRIVATE ..)

target_compile_definitions(CyclesRender
  PRIVATE
  GLOG_NO_ABBREVIATED_SEVERITIES=1
  OSL_DEBUG=$<CONFIG:DEBUG>
  TBB_USE_DEBUG=$<CONFIG:DEBUG>
)

target_link_libraries(CyclesRender PRIVATE cycles_hydra)

delayed_install("" $<TARGET_FILE:CyclesRender> ${CYCLES_INSTALL_PATH})
delayed_install(${CMAKE_CURRENT_SOURCE_DIR} "init.py;menu.py" ${CYCLES_INSTALL_PATH})

if(WIN32)
  # Bundled libraries that Nuke loads too, in another version. Shared on purpose: TBB (USD and
  # Cycles must use Nuke's scheduler) and the MSVC runtime.
  set(_nuke_private_dlls Iex.dll IlmThread.dll Imath.dll OpenEXR.dll OpenEXRCore.dll OpenEXRUtil.dll)
  install(CODE "
    execute_process(
      COMMAND \"${NUKE_ROOT}/python.exe\" -I \"${CMAKE_CURRENT_SOURCE_DIR}/rename_dlls.py\"
              \"${CYCLES_INSTALL_PATH}\" ${_nuke_private_dlls}
      COMMAND_ERROR_IS_FATAL ANY)
  ")
  unset(_nuke_private_dlls)
endif()
