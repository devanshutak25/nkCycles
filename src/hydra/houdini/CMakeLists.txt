# SPDX-FileCopyrightText: 2024 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

#####################################################################
# Houdini integration: package, renderer registration and shader nodes
#####################################################################

# Package file that adds the Cycles installation to the Houdini path.
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/packages/cycles.json.in
  ${CMAKE_CURRENT_BINARY_DIR}/packages/cycles.json
  @ONLY
)
delayed_install("${CMAKE_CURRENT_BINARY_DIR}/packages" "cycles.json" "${CMAKE_INSTALL_PREFIX}/houdini/packages")

# Register the delegate with Houdini, so it is listed in Solaris and husk.
delayed_install("${CMAKE_CURRENT_SOURCE_DIR}" "UsdRenderers.json" "${CMAKE_INSTALL_PREFIX}/houdini")

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/icons/ DESTINATION houdini/icons)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/toolbar/ DESTINATION houdini/toolbar)

#####################################################################
# Shader node registry dump, used to generate a VOP HDA per Cycles shader node
#####################################################################

set(LIB
  cycles_session
  cycles_scene
  cycles_device
  cycles_kernel
  cycles_bvh
  cycles_subd
  cycles_integrator
  cycles_graph
  cycles_util
)
if(WITH_CYCLES_OSL)
  list(APPEND LIB cycles_kernel_osl)
endif()
cycles_external_libraries_append(LIB)

add_executable(shaders_node_dump src/shaders.cpp)
target_include_directories(shaders_node_dump PRIVATE ../..)
target_link_libraries(shaders_node_dump PRIVATE ${LIB})
target_compile_definitions(shaders_node_dump PRIVATE $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX=1>)

# Run the tool against Houdini's shared libraries, which it links against.
if(WIN32)
  list(JOIN PLATFORM_BUNDLED_LIBRARY_DIRS ";" _bundled_dirs)
  set(_shaders_node_dump_env "PATH=${HOUDINI_ROOT}/bin;${_bundled_dirs}")
  unset(_bundled_dirs)
elseif(APPLE)
  set(_shaders_node_dump_env "DYLD_LIBRARY_PATH=${HOUDINI_ROOT}/Frameworks/Houdini.framework/Libraries")
else()
  set(_shaders_node_dump_env "LD_LIBRARY_PATH=${HOUDINI_ROOT}/dsolib:$ENV{LD_LIBRARY_PATH}")
endif()

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/shader_nodes.json
         ${CMAKE_CURRENT_BINARY_DIR}/render_settings.json
  COMMAND ${CMAKE_COMMAND} -E env "${_shaders_node_dump_env}" $<TARGET_FILE:shaders_node_dump>
  DEPENDS shaders_node_dump
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  COMMENT "Dumping Cycles shader nodes and render settings"
  VERBATIM
)
add_custom_target(shaders_node_dump_json ALL DEPENDS
  ${CMAKE_CURRENT_BINARY_DIR}/shader_nodes.json
  ${CMAKE_CURRENT_BINARY_DIR}/render_settings.json
)
unset(_shaders_node_dump_env)

# Generate the HDAs and render settings UI with hython at install time, as this requires a
# Houdini license.
if(WIN32)
  set(HYTHON_EXECUTABLE "${HOUDINI_ROOT}/bin/hython.exe")
elseif(APPLE)
  set(HYTHON_EXECUTABLE "${HOUDINI_ROOT}/Frameworks/Houdini.framework/Resources/bin/hython")
else()
  set(HYTHON_EXECUTABLE "${HOUDINI_ROOT}/bin/hython")
endif()

install(CODE "
  message(STATUS \"Generating Cycles shader HDAs\")
  execute_process(
    COMMAND \"${HYTHON_EXECUTABLE}\"
            \"${CMAKE_CURRENT_SOURCE_DIR}/src/shader2hda.py\"
            --json \"${CMAKE_CURRENT_BINARY_DIR}/shader_nodes.json\"
            --hda-install-dir \"\${CMAKE_INSTALL_PREFIX}/houdini/otls\"
    RESULT_VARIABLE _hda_result
  )
  if(NOT _hda_result EQUAL 0)
    message(WARNING \"shader2hda.py failed with exit code \${_hda_result}\")
  endif()

  message(STATUS \"Generating Cycles render settings UI\")
  execute_process(
    COMMAND \"${HYTHON_EXECUTABLE}\"
            \"${CMAKE_CURRENT_SOURCE_DIR}/src/rendersettings2ds.py\"
            --json \"${CMAKE_CURRENT_BINARY_DIR}/render_settings.json\"
            --ds-install-dir \"\${CMAKE_INSTALL_PREFIX}/houdini/soho/parameters\"
    RESULT_VARIABLE _ds_result
  )
  if(NOT _ds_result EQUAL 0)
    message(WARNING \"rendersettings2ds.py failed with exit code \${_ds_result}\")
  endif()
")
