# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Find USD libraries in a Nuke installation (Nuke 15 and newer).
# Variables are matching those output by FindUSDPixar.cmake.
#
# Nuke ships its USD libraries with a "fn" prefix (fntf, fnusd, ...) and the default
# pxrInternal_vX_YY namespace, but no USD headers. Headers of the matching USD release
# must be given with NUKE_USD_INCLUDE_DIR (a USD source tree with a generated pxr/pxr.h),
# and Boost headers with NUKE_BOOST_INCLUDE_DIR (Nuke renames the boost namespace to
# "foundryboost").

if(NUKE_ROOT AND EXISTS ${NUKE_ROOT})
  message(STATUS "Found Nuke: ${NUKE_ROOT}")

  if(NOT NUKE_USD_INCLUDE_DIR OR NOT EXISTS ${NUKE_USD_INCLUDE_DIR}/pxr/pxr.h)
    message(SEND_ERROR "NUKE_USD_INCLUDE_DIR must point to USD headers with pxr/pxr.h")
  endif()

  set(USD_FOUND ON)
  set(NUKE_FOUND ON)

  set(_library_dir ${NUKE_ROOT})
  set(_include_dir ${NUKE_ROOT}/include)

  # Version, from the NDK.
  file(STRINGS "${_include_dir}/DDImage/ddImageVersionNumbers.h" _nuke_version_major
    REGEX "^#define kDDImageVersionMajorNum[ \t].*$")
  string(REGEX MATCHALL "[0-9]+" NUKE_VERSION_MAJOR ${_nuke_version_major})
  unset(_nuke_version_major)
  message(STATUS "Nuke version: ${NUKE_VERSION_MAJOR}")

  # USD
  set(USD_LIBRARIES hd hdsi hgi hgiGL gf arch garch plug tf trace vt work sdf cameraUtil hf js pxOsd usd usdImaging usdGeom)

  foreach(lib ${USD_LIBRARIES})
    find_library(_pxr_library NAMES fn${lib} PATHS ${_library_dir} NO_DEFAULT_PATH)
    if(NOT _pxr_library)
      message(SEND_ERROR "Nuke USD library fn${lib} not found in ${_library_dir}")
    endif()
    add_library(${lib} SHARED IMPORTED)
    set_property(TARGET ${lib} APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
    set_target_properties(${lib} PROPERTIES
      IMPORTED_LOCATION_RELEASE "${_pxr_library}"
      IMPORTED_IMPLIB_RELEASE "${_pxr_library}"
    )
    unset(_pxr_library CACHE)
  endforeach()

  # Python (USD is built with Python support). Nuke ships no import libraries for Python and
  # Boost.Python, so they are generated from the DLLs (NUKE_IMPLIB_DIR).
  find_path(_python_include_dir NAMES "pyconfig.h" PATHS ${_include_dir}/include NO_DEFAULT_PATH)
  find_library(_python_library NAMES python310 python311 python312 python313
    PATHS ${NUKE_IMPLIB_DIR} ${_library_dir} NO_DEFAULT_PATH)
  file(GLOB _boost_python_library "${NUKE_IMPLIB_DIR}/foundryboost_python*.lib")

  set(USD_INCLUDE_DIR ${NUKE_USD_INCLUDE_DIR})
  list(APPEND USD_INCLUDE_DIRS ${NUKE_USD_INCLUDE_DIR} ${_python_include_dir} ${NUKE_BOOST_INCLUDE_DIR})
  list(APPEND USD_LIBRARIES ${_python_library} ${_boost_python_library})
  set(USD_OVERRIDE_PYTHON ON)

  unset(_python_include_dir CACHE)
  unset(_python_library CACHE)
  unset(_boost_python_library)

  # Boost: Nuke's is built with the namespace renamed to "foundryboost".
  # Header-only use, except Boost.Python symbols which must match Nuke's DLL.
  add_definitions(-DBOOST_ALL_NO_LIB -Dboost=foundryboost)
  # Also used by the precompiled OpenVDB headers.
  include_directories(SYSTEM ${NUKE_BOOST_INCLUDE_DIR})

  # Nuke loads its own MSVC runtime, which may be older than the compiler. Since MSVC 14.40
  # std::mutex has a constexpr constructor that crashes with older msvcp140.dll.
  if(MSVC)
    add_definitions(-D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
  endif()

  # Nuke has no MaterialX in its USD.
  set(USD_OVERRIDE_MATERIALX ON)

  # TBB: Nuke 15 uses TBB 2020.3, the same as the legacy precompiled libraries (WITH_LEGACY_LIBRARIES),
  # whose tbb.dll resolves to the one Nuke already loaded.

  # Cleanup
  unset(_library_dir)
  unset(_include_dir)
else()
  message(SEND_ERROR "Did not find Nuke at ${NUKE_ROOT}")
endif()
