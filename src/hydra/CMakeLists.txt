# SPDX-FileCopyrightText: 2022 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

#####################################################################
# Cycles Hydra render delegate
#####################################################################

set(INC
  ..
)
set(INC_SYS
)

set(LIB
  cycles_scene
  cycles_session
  cycles_graph
  PUBLIC bf::dependencies::openimageio
  bf::dependencies::epoxy
)
cycles_external_libraries_append(LIB)

set(SRC_HD_CYCLES_HEADERS
  camera.h
  config.h
  curves.h
  field.h
  file_reader.h
  geometry.h
  geometry.inl
  instancer.h
  light.h
  material.h
  mesh.h
  node_util.h
  output_driver.h
  pointcloud.h
  render_buffer.h
  render_delegate.h
  render_pass.h
  session.h
  util.h
  volume.h
)

set(SRC_HD_CYCLES
  curves.cpp
  camera.cpp
  field.cpp
  file_reader.cpp
  geometry.cpp
  instancer.cpp
  light.cpp
  material.cpp
  mesh.cpp
  node_util.cpp
  output_driver.cpp
  pointcloud.cpp
  render_buffer.cpp
  render_delegate.cpp
  render_pass.cpp
  session.cpp
  util.cpp
  volume.cpp
)

# Blender libraries do not include hgiGL, so build without display driver then.
if(EXISTS ${USD_INCLUDE_DIR}/pxr/imaging/hgiGL)
  add_definitions(-DWITH_HYDRA_DISPLAY_DRIVER)
  list(APPEND SRC_HD_CYCLES display_driver.cpp)
  list(APPEND SRC_HD_CYCLES_HEADERS display_driver.h)
endif()

# MaterialX materials, translated with the MaterialX libraries USD was built with.
if(TARGET hdMtlx)
  add_definitions(-DWITH_HYDRA_MATERIALX)
  list(APPEND SRC_HD_CYCLES materialx.cpp)
  list(APPEND SRC_HD_CYCLES_HEADERS materialx.h)
endif()

# Volumes from SOPs in a Houdini LOP stage, read through Houdini's libraries.
if(HOUDINI_FOUND AND WITH_OPENVDB)
  list(APPEND SRC_HD_CYCLES houdini_volume.cpp)
  list(APPEND SRC_HD_CYCLES_HEADERS houdini_volume.h)
  add_definitions(-DWITH_HOUDINI_VOLUMES)
  # Houdini Development Kit definitions, see toolkit/cmake/HoudiniConfig.cmake.
  set(_hdk_definitions
    SIZEOF_VOID_P=8 SESI_LITTLE_ENDIAN UT_ASSERT_LEVEL=0 MAKING_DSO
    FBX_ENABLED=1 OPENCL_ENABLED=1 OPENVDB_ENABLED=1 USE_VULKAN USE_QT6=1
    HBOOST_ALL_NO_LIB HBOOST_ASIO_USE_TS_EXECUTOR_AS_DEFAULT=1 HBOOST_BIND_GLOBAL_PLACEHOLDERS=1
    EIGEN_MALLOC_ALREADY_ALIGNED=0 _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS=1 _USE_MATH_DEFINES
  )
  if(WIN32)
    list(APPEND _hdk_definitions WIN32 AMD64 I386 SWAP_BITFIELDS STRICT WIN32_LEAN_AND_MEAN)
  elseif(APPLE)
    list(APPEND _hdk_definitions MBSD MBSD_COCOA MBSD_INTEL)
  else()
    list(APPEND _hdk_definitions LINUX AMD64 _GNU_SOURCE)
  endif()
  set_source_files_properties(houdini_volume.cpp PROPERTIES COMPILE_DEFINITIONS "${_hdk_definitions}")
  unset(_hdk_definitions)
  # Houdini libraries used by the VDB and volume primitives and their headers.
  foreach(_hdk_lib GT GU GEO UT CE)
    find_library(_hdk_library NAMES lib${_hdk_lib} HOUDINI_${_hdk_lib}
      PATHS ${HOUDINI_ROOT}/custom/houdini/dsolib ${HOUDINI_ROOT}/dsolib
      ${HOUDINI_ROOT}/Frameworks/Houdini.framework/Libraries NO_DEFAULT_PATH)
    list(APPEND LIB ${_hdk_library})
    unset(_hdk_library CACHE)
  endforeach()
endif()

# Silence warning from USD headers using deprecated TBB header.
add_definitions(
  -D__TBB_show_deprecation_message_atomic_H
  -D__TBB_show_deprecation_message_task_H
)

include_directories(${INC})
include_directories(SYSTEM ${INC_SYS})

add_library(cycles_hydra STATIC
  ${SRC_HD_CYCLES}
  ${SRC_HD_CYCLES_HEADERS}
)

target_compile_options(cycles_hydra
  PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/wd4003 /wd4244 /wd4506>
  $<$<CXX_COMPILER_ID:GNU>:-Wno-float-conversion -Wno-double-promotion -Wno-deprecated>
)

target_compile_definitions(cycles_hydra
  PRIVATE
  GLOG_NO_ABBREVIATED_SEVERITIES=1
  OSL_DEBUG=$<CONFIG:DEBUG>
  TBB_USE_DEBUG=$<CONFIG:DEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX=1>
)

target_link_libraries(cycles_hydra
  PUBLIC
  bf::dependencies::openimageio
  bf::dependencies::optional::tbb
  bf::dependencies::optional::usd
  bf::dependencies::optional::python
  PRIVATE
  ${LIB}
)

if(WITH_CYCLES_HYDRA_RENDER_DELEGATE)
  set(SRC_HD_CYCLES_PLUGIN
    plugin.h
    plugin.cpp
  )
  set(HdCyclesPluginName hdCycles)
  add_library(${HdCyclesPluginName} SHARED ${SRC_HD_CYCLES_PLUGIN})

  set_target_properties(${HdCyclesPluginName}
    PROPERTIES PREFIX ""
  )

  target_compile_definitions(${HdCyclesPluginName}
    PRIVATE
    MFB_PACKAGE_NAME=${HdCyclesPluginName}
    MFB_ALT_PACKAGE_NAME=${HdCyclesPluginName}
    GLOG_NO_ABBREVIATED_SEVERITIES=1
    OSL_DEBUG=$<CONFIG:DEBUG>
    TBB_USE_DEBUG=$<CONFIG:DEBUG>
    $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX=1>
  )

  target_link_libraries(${HdCyclesPluginName}
    cycles_hydra
  )

  if(APPLE)
    set_property(
      TARGET
      ${HdCyclesPluginName}
      APPEND_STRING PROPERTY LINK_FLAGS
      " -Wl,-exported_symbols_list,'${CMAKE_CURRENT_SOURCE_DIR}/resources/apple_symbols.map'"
    )
  elseif(UNIX)
    set_property(
      TARGET
      ${HdCyclesPluginName}
      APPEND_STRING PROPERTY LINK_FLAGS
      " -Wl,--version-script='${CMAKE_CURRENT_SOURCE_DIR}/resources/linux_symbols.map'"
    )
  endif()


  if(WITH_CYCLES_BLENDER)
    # Install inside add-on
    set(CYCLES_HYDRA_INSTALL_PATH ${CYCLES_INSTALL_PATH}/hydra)
  elseif(HOUDINI_FOUND)
    set(CYCLES_HYDRA_INSTALL_PATH ${CYCLES_INSTALL_PATH}/houdini/dso)
  else()
    # Install next to cycles executable
    set(CYCLES_HYDRA_INSTALL_PATH ${CMAKE_INSTALL_PREFIX}/hydra)
  endif()

  if(HOUDINI_FOUND)
    delayed_install("${CMAKE_CURRENT_SOURCE_DIR}" "plugInfo.json" ${CYCLES_HYDRA_INSTALL_PATH}/usd_plugins)
    delayed_install("" $<TARGET_FILE:${HdCyclesPluginName}> ${CYCLES_HYDRA_INSTALL_PATH}/usd)
  else()
    # Put the root `plugInfo.json` one level up.
    delayed_install("${CMAKE_CURRENT_SOURCE_DIR}" "plugInfo.json" ${CYCLES_HYDRA_INSTALL_PATH})
    delayed_install("" $<TARGET_FILE:${HdCyclesPluginName}> ${CYCLES_HYDRA_INSTALL_PATH})
  endif()

  set_target_properties(${HdCyclesPluginName}
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  set(PLUG_INFO_ROOT "..")

  if(WITH_CYCLES_BLENDER)
    # Full path not yet known at this point and RELATIVE_PATH requires
    # absolute path as input. So just set manually.
    set(PLUG_INFO_LIBRARY_PATH "../${HdCyclesPluginName}${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(PLUG_INFO_RESOURCE_PATH "../..")
  else()
    if (HOUDINI_FOUND)
      file(RELATIVE_PATH
        PLUG_INFO_LIBRARY_PATH
        "${CYCLES_HYDRA_INSTALL_PATH}/usd_plugins/${HdCyclesPluginName}"
        "${CYCLES_HYDRA_INSTALL_PATH}/usd/${HdCyclesPluginName}${CMAKE_SHARED_LIBRARY_SUFFIX}")
      # Relative to the plugin root, which is "usd_plugins/hdCycles" for Houdini.
      file(RELATIVE_PATH PLUG_INFO_RESOURCE_PATH
        "${CYCLES_HYDRA_INSTALL_PATH}/usd_plugins/${HdCyclesPluginName}"
        "${CYCLES_INSTALL_PATH}")
    else()
      file(RELATIVE_PATH
        PLUG_INFO_LIBRARY_PATH
        "${CYCLES_HYDRA_INSTALL_PATH}/${HdCyclesPluginName}"
        "${CYCLES_HYDRA_INSTALL_PATH}/${HdCyclesPluginName}${CMAKE_SHARED_LIBRARY_SUFFIX}")
      file(RELATIVE_PATH PLUG_INFO_RESOURCE_PATH
        "${CYCLES_HYDRA_INSTALL_PATH}/${HdCyclesPluginName}"
        "${CYCLES_INSTALL_PATH}")
    endif()
  endif()

  configure_file(resources/plugInfo.json
    ${CMAKE_CURRENT_BINARY_DIR}/resources/plugInfo.json
    @ONLY
  )

  if(HOUDINI_FOUND)
    delayed_install("${CMAKE_CURRENT_BINARY_DIR}/resources" "plugInfo.json" "${CYCLES_HYDRA_INSTALL_PATH}/usd_plugins/${HdCyclesPluginName}/resources")
    # Houdini package, renderer registration and shader nodes.
    add_subdirectory(houdini)
  else()
    delayed_install("${CMAKE_CURRENT_BINARY_DIR}/resources" "plugInfo.json" "${CYCLES_HYDRA_INSTALL_PATH}/${HdCyclesPluginName}/resources")
  endif()

  if(WITH_CYCLES_BLENDER)
    get_filename_component(_addons_core_dir "${CYCLES_INSTALL_PATH}" DIRECTORY)
    delayed_install(
      "${CMAKE_CURRENT_SOURCE_DIR}/addon"
      "__init__.py"
      "${_addons_core_dir}/hydra_cycles"
    )
    unset(_addons_core_dir)
  endif()
endif()
