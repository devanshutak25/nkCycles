# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Find USD libraries in a Nuke installation (Nuke 15 and newer).
# Variables are matching those output by FindUSDPixar.cmake.
#
# Nuke 15.1 and newer ship USD headers (with Foundry's changes) and import libraries in FnUSD.
# Nuke 15.0 ships its USD libraries with a "fn" prefix (fntf, fnusd, ...) but no USD headers:
# headers of the matching USD release must be given with NUKE_USD_INCLUDE_DIR (a USD source tree
# with a generated pxr/pxr.h).
#
# USD versions before 25.05 use Boost: its headers are given with NUKE_BOOST_INCLUDE_DIR (Nuke
# renames the boost namespace to "foundryboost").

if(NUKE_ROOT AND EXISTS ${NUKE_ROOT})
  message(STATUS "Found Nuke: ${NUKE_ROOT}")

  if(EXISTS ${NUKE_ROOT}/FnUSD/include/pxr/pxr.h)
    set(_usd_include_dir ${NUKE_ROOT}/FnUSD/include)
    set(_usd_library_dir ${NUKE_ROOT}/FnUSD/lib)
    set(_usd_library_prefix usd_)
  elseif(NUKE_USD_INCLUDE_DIR AND EXISTS ${NUKE_USD_INCLUDE_DIR}/pxr/pxr.h)
    set(_usd_include_dir ${NUKE_USD_INCLUDE_DIR})
    set(_usd_library_dir ${NUKE_ROOT})
    set(_usd_library_prefix fn)
  else()
    message(SEND_ERROR "Nuke has no USD headers, NUKE_USD_INCLUDE_DIR must point to USD headers with pxr/pxr.h")
  endif()

  file(STRINGS "${_usd_include_dir}/pxr/pxr.h" _pxr_version REGEX "^#define PXR_VERSION .*$")
  string(REGEX MATCHALL "[0-9]+" PXR_VERSION "${_pxr_version}")
  unset(_pxr_version)
  message(STATUS "Nuke USD version: ${PXR_VERSION}")
  # USD's own copy of Boost.Python (pxr_boost::python) instead of Boost's.
  file(STRINGS "${_usd_include_dir}/pxr/pxr.h" _pxr_internal_boost_python
    REGEX "^#define PXR_USE_INTERNAL_BOOST_PYTHON")

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
  if(_pxr_internal_boost_python)
    list(APPEND USD_LIBRARIES python)
  endif()

  foreach(lib ${USD_LIBRARIES})
    find_library(_pxr_library NAMES ${_usd_library_prefix}${lib} PATHS ${_usd_library_dir}
      NO_DEFAULT_PATH)
    if(NOT _pxr_library)
      message(SEND_ERROR "Nuke USD library ${_usd_library_prefix}${lib} not found in ${_usd_library_dir}")
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

  set(USD_INCLUDE_DIR ${_usd_include_dir})
  list(APPEND USD_INCLUDE_DIRS ${_usd_include_dir} ${_python_include_dir} ${NUKE_BOOST_INCLUDE_DIR})
  list(APPEND USD_LIBRARIES ${_python_library})
  if(NOT _pxr_internal_boost_python)
    list(APPEND USD_LIBRARIES ${_boost_python_library})
  endif()
  set(USD_OVERRIDE_PYTHON ON)

  unset(_python_include_dir CACHE)
  unset(_python_library CACHE)
  unset(_boost_python_library)

  # Boost: Nuke's is built with the namespace renamed to "foundryboost".
  # Header-only use, except Boost.Python symbols which must match Nuke's DLL.
  if(NUKE_BOOST_INCLUDE_DIR)
    add_definitions(-DBOOST_ALL_NO_LIB -Dboost=foundryboost)
    # Also used by the precompiled OpenVDB headers.
    include_directories(SYSTEM ${NUKE_BOOST_INCLUDE_DIR})
  elseif(PXR_VERSION VERSION_LESS 2505)
    message(SEND_ERROR "USD before 25.05 needs Boost headers in NUKE_BOOST_INCLUDE_DIR")
  endif()

  # Nuke loads its own MSVC runtime, which may be older than the compiler. Since MSVC 14.40
  # std::mutex has a constexpr constructor that crashes with older msvcp140.dll.
  if(MSVC)
    add_definitions(-D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
  endif()

  # MaterialX: none in Nuke 15 and 16, Nuke 17 has no MaterialX headers or import libraries.
  set(USD_OVERRIDE_MATERIALX ON)

  # TBB: Nuke 15 and 16 use TBB 2020.3, the same as the legacy precompiled libraries, whose
  # tbb.dll resolves to the one Nuke already loaded. Nuke 17 uses oneTBB 2021.13, like the
  # Blender 5.0 libraries (CYCLES_LIB_DIR).
  if(WIN32 AND NOT EXISTS ${NUKE_ROOT}/tbb.dll AND NOT CYCLES_LIB_DIR)
    message(SEND_ERROR "Nuke has no TBB 2020 (tbb.dll): set CYCLES_LIB_DIR to the precompiled "
      "libraries of the Blender release with the same oneTBB (Blender 5.0 for Nuke 17)")
  endif()

  # Cleanup
  unset(_library_dir)
  unset(_include_dir)
  unset(_usd_include_dir)
  unset(_usd_library_dir)
  unset(_usd_library_prefix)
  unset(_pxr_internal_boost_python)
else()
  message(SEND_ERROR "Did not find Nuke at ${NUKE_ROOT}")
endif()
