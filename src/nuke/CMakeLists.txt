# SPDX-FileCopyrightText: 2026 Devanshu Tak
#
# SPDX-License-Identifier: Apache-2.0

#####################################################################
# Nuke render node
#####################################################################

if(NUKE_FOUND)
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
    # Bundled libraries the node imports, and the OpenImageDenoise device modules loaded at runtime.
    # Embree, OSL and SYCL ones are only in the list when those are enabled.
    # TBB and the MSVC runtime are Nuke's.
    set(_nuke_runtime_libs ${PLATFORM_BUNDLED_LIBRARIES_RELEASE})
    set(_nuke_runtime_names "OpenImageIO|OpenImageIO_Util|OpenEXR|OpenEXRCore|Iex|IlmThread|[Ii]math|OpenColorIO_[0-9_]+|openvdb|epoxy-0|OpenImageDenoise|OpenImageDenoise_core|OpenImageDenoise_device_(cpu|cuda)|boost_thread-vc[0-9]+-mt-x64-[0-9_]+|embree4|osl(comp|exec|noise|query)")
    if(EMBREE_SYCL_SUPPORT)
      # The shared Embree with SYCL support imports the SYCL runtime.
      string(APPEND _nuke_runtime_names "|sycl[0-9]+|pi_[a-z0-9_]+|ur_[a-z0-9_]+")
    endif()
    list(FILTER _nuke_runtime_libs INCLUDE REGEX "/(${_nuke_runtime_names})[.]dll$")
    install(FILES ${_nuke_runtime_libs} DESTINATION ${CYCLES_INSTALL_PATH})
    unset(_nuke_runtime_libs)
    unset(_nuke_runtime_names)

    # Bundled libraries that Nuke loads too, in another version. Shared on purpose: TBB (USD and
    # Cycles must use Nuke's scheduler) and the MSVC runtime.
    set(_nuke_private_dlls Iex.dll IlmThread.dll Imath.dll OpenEXR.dll OpenEXRCore.dll)
    if(WITH_CYCLES_OPENVDB)
      list(APPEND _nuke_private_dlls openvdb.dll)
    endif()
    install(CODE "
      execute_process(
        COMMAND \"${NUKE_ROOT}/python.exe\" -I \"${CMAKE_CURRENT_SOURCE_DIR}/rename_dlls.py\"
                \"${CYCLES_INSTALL_PATH}\" ${_nuke_private_dlls}
        COMMAND_ERROR_IS_FATAL ANY)
    ")
    unset(_nuke_private_dlls)
  endif()
endif()

#####################################################################
# Test program: renders USD files like the node, without Nuke
#####################################################################

if(WITH_CYCLES_NUKE_RENDER_TEST)
  add_executable(nkcycles_render_test
    render.cpp
    render.h
    render_test.cpp
  )

  target_include_directories(nkcycles_render_test PRIVATE ..)

  target_compile_definitions(nkcycles_render_test
    PRIVATE
    GLOG_NO_ABBREVIATED_SEVERITIES=1
    OSL_DEBUG=$<CONFIG:DEBUG>
    TBB_USE_DEBUG=$<CONFIG:DEBUG>
  )

  target_link_libraries(nkcycles_render_test PRIVATE cycles_hydra)

  install(TARGETS nkcycles_render_test DESTINATION ${CMAKE_INSTALL_PREFIX})
  if(NOT NUKE_FOUND)
    cycles_install_libraries(nkcycles_render_test)
    if(USD_LIBRARY_DIR)
      install(DIRECTORY ${USD_LIBRARY_DIR}/usd DESTINATION ${CMAKE_INSTALL_PREFIX})
    endif()
  endif()
endif()
