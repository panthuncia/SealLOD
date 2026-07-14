# CMakeList.txt : Top-level CMake project file, do global configuration
# and include sub-projects here.
#
cmake_minimum_required (VERSION 3.10)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
if(POLICY CMP0177)
  cmake_policy(SET CMP0177 NEW)
endif()

foreach(_basicrenderer_cached_tool
    CMAKE_C_COMPILER
    CMAKE_CXX_COMPILER
    CMAKE_LINKER
    CMAKE_AR
    CMAKE_MT
    CMAKE_RC_COMPILER
    Z_VCPKG_CL)
  if(DEFINED ${_basicrenderer_cached_tool}
      AND IS_ABSOLUTE "${${_basicrenderer_cached_tool}}"
      AND NOT EXISTS "${${_basicrenderer_cached_tool}}")
    message(STATUS "Clearing stale cached tool path ${_basicrenderer_cached_tool}: ${${_basicrenderer_cached_tool}}")
    unset(${_basicrenderer_cached_tool} CACHE)
  endif()
endforeach()

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR CMAKE_TOOLCHAIN_FILE STREQUAL "")
  set(_basicrenderer_vcpkg_toolchain "")

  if(DEFINED ENV{VCPKG_ROOT})
    set(_basicrenderer_vcpkg_toolchain "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
  endif()

  if(CMAKE_HOST_WIN32 AND NOT EXISTS "${_basicrenderer_vcpkg_toolchain}")
    file(GLOB _basicrenderer_vs_vcpkg_toolchains
      "C:/Program Files/Microsoft Visual Studio/*/*/VC/vcpkg/scripts/buildsystems/vcpkg.cmake"
    )
    if(_basicrenderer_vs_vcpkg_toolchains)
      list(GET _basicrenderer_vs_vcpkg_toolchains 0 _basicrenderer_vcpkg_toolchain)
    endif()
  endif()

  if(EXISTS "${_basicrenderer_vcpkg_toolchain}")
    set(CMAKE_TOOLCHAIN_FILE "${_basicrenderer_vcpkg_toolchain}" CACHE FILEPATH "")
  elseif(DEFINED ENV{VCPKG_ROOT})
    message(FATAL_ERROR "VCPKG_ROOT is set, but the vcpkg toolchain file was not found at '${_basicrenderer_vcpkg_toolchain}'.")
  else()
    message(FATAL_ERROR "Could not find vcpkg. Set VCPKG_ROOT or pass -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake.")
  endif()
endif()

# Enable debug information for MSVC builds.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
endif()

if (POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

project ("BasicRenderer")

if(CMAKE_MAP_IMPORTED_CONFIG_DEBUG STREQUAL "RelWithDebInfo;Release")
  unset(CMAKE_MAP_IMPORTED_CONFIG_DEBUG CACHE)
endif()

if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_definitions($<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=0>)
endif()

function(basicrenderer_use_release_library_for_debug target_name)
  if(NOT TARGET ${target_name})
    return()
  endif()

  # vcpkg's app-local deployment searches the Debug bin directory for Debug
  # builds.  Linking the ASan build against release fmt makes the executable
  # import fmt.dll, while deployment provides the Debug runtime, fmtd.dll.
  # Keep fmt's link configuration aligned with app-local deployment for ASan.
  if(ENABLE_ADDRESS_SANITIZER AND target_name STREQUAL "fmt::fmt")
    return()
  endif()

  set_target_properties(${target_name} PROPERTIES MAP_IMPORTED_CONFIG_DEBUG Release)
  foreach(_basicrenderer_imported_prop IN ITEMS IMPORTED_LOCATION IMPORTED_IMPLIB)
    get_target_property(_basicrenderer_imported_release_value ${target_name} ${_basicrenderer_imported_prop}_RELEASE)
    if(_basicrenderer_imported_release_value)
      set_target_properties(${target_name} PROPERTIES ${_basicrenderer_imported_prop}_DEBUG "${_basicrenderer_imported_release_value}")
    endif()
  endforeach()
endfunction()

if(MSVC)
  # Keep compiler debug info in .obj files (/Z7). This avoids large shared
  # compiler PDBs, which can hit MSVC/PDB API limits before the final link.
  # The linker still emits an executable PDB when /DEBUG is enabled.
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
    "$<IF:$<CONFIG:Debug,RelWithDebInfo>,Embedded,>"
  )

  foreach(_basicrenderer_debug_info_config DEBUG RELWITHDEBINFO)
    foreach(_basicrenderer_debug_info_lang C CXX)
      set(_basicrenderer_debug_info_flags_var
        "CMAKE_${_basicrenderer_debug_info_lang}_FLAGS_${_basicrenderer_debug_info_config}"
      )
      if(DEFINED ${_basicrenderer_debug_info_flags_var})
        string(REPLACE "/Zi" "/Z7" ${_basicrenderer_debug_info_flags_var} "${${_basicrenderer_debug_info_flags_var}}")
        string(REPLACE "/ZI" "/Z7" ${_basicrenderer_debug_info_flags_var} "${${_basicrenderer_debug_info_flags_var}}")
        set(${_basicrenderer_debug_info_flags_var}
          "${${_basicrenderer_debug_info_flags_var}}"
          CACHE STRING "MSVC ${_basicrenderer_debug_info_config} ${_basicrenderer_debug_info_lang} flags" FORCE
        )
      endif()
    endforeach()
  endforeach()
endif()

option(BASICRHI_ENABLE_RESHAPE "Enable GPU-Reshape-backed debug instrumentation in BasicRHI" OFF)
option(BASICRENDERER_BUILD_BRNIFLY "Build the GPL-separated BRNifly NIF middleware executable" ON)
set(BASICRENDERER_VULKAN_INCLUDE_DIR "" CACHE PATH "Preferred Vulkan SDK include directory. Leave empty to auto-detect a Vulkan SDK with VK_EXT_descriptor_heap.")

function(basicrenderer_detect_vulkan_headers out_var)
  set(_basicrenderer_vulkan_include_candidates)

  if(BASICRENDERER_VULKAN_INCLUDE_DIR)
    list(APPEND _basicrenderer_vulkan_include_candidates "${BASICRENDERER_VULKAN_INCLUDE_DIR}")
  endif()

  if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
    list(APPEND _basicrenderer_vulkan_include_candidates "$ENV{VULKAN_SDK}/Include")
  endif()

  if(CMAKE_HOST_WIN32)
    file(GLOB _basicrenderer_vulkan_sdk_dirs LIST_DIRECTORIES true "C:/VulkanSDK/*")
    list(SORT _basicrenderer_vulkan_sdk_dirs COMPARE NATURAL ORDER DESCENDING)
    foreach(_basicrenderer_vulkan_sdk_dir IN LISTS _basicrenderer_vulkan_sdk_dirs)
      list(APPEND _basicrenderer_vulkan_include_candidates "${_basicrenderer_vulkan_sdk_dir}/Include")
    endforeach()
  endif()

  foreach(_basicrenderer_vulkan_include IN LISTS _basicrenderer_vulkan_include_candidates)
    set(_basicrenderer_vulkan_core "${_basicrenderer_vulkan_include}/vulkan/vulkan_core.h")
    if(EXISTS "${_basicrenderer_vulkan_core}")
      file(STRINGS "${_basicrenderer_vulkan_core}" _basicrenderer_vulkan_has_descriptor_heap
        REGEX "^#define VK_EXT_descriptor_heap 1$")
      if(_basicrenderer_vulkan_has_descriptor_heap)
        set(${out_var} "${_basicrenderer_vulkan_include}" PARENT_SCOPE)
        return()
      endif()
    endif()
  endforeach()

  set(${out_var} "" PARENT_SCOPE)
endfunction()

basicrenderer_detect_vulkan_headers(BASICRENDERER_DETECTED_VULKAN_INCLUDE_DIR)
if(BASICRENDERER_DETECTED_VULKAN_INCLUDE_DIR)
  set(Vulkan_INCLUDE_DIR "${BASICRENDERER_DETECTED_VULKAN_INCLUDE_DIR}" CACHE PATH "Vulkan include directory" FORCE)
  message(STATUS "BasicRenderer: using Vulkan headers from ${BASICRENDERER_DETECTED_VULKAN_INCLUDE_DIR}")
endif()

if (WIN32)
  add_compile_definitions(NOMINMAX)
endif()

if (MSVC)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
  add_compile_definitions(_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/bigobj>)
endif()

set(_basicrenderer_clang_warning_options
  -Werror
  -Wno-error=deprecated-declarations
  -Wno-error=deprecated-volatile
  -Wno-unused-command-line-argument
  -Wno-unused-function
  -Wno-switch
  -Wno-reorder-ctor
  -Wno-unused-variable
  -Wno-unused-but-set-variable
  -Wno-unused-private-field
  -Wno-misleading-indentation
  -Wno-nonportable-include-path
  -Wno-inconsistent-missing-override
  -Wno-format
  -Wno-implicit-float-conversion
  -Wno-missing-braces
  -Wno-braced-scalar-init
  -Wno-pessimizing-move
  -Wno-self-assign-field
  -Wno-unused-lambda-capture
  "-Wno-#pragma-messages"
)
foreach(_basicrenderer_clang_warning_option IN LISTS _basicrenderer_clang_warning_options)
  add_compile_options($<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:Clang>>:${_basicrenderer_clang_warning_option}>)
endforeach()

#set(CMAKE_VS_JUST_MY_CODE_DEBUGGING ON) # Skip external code in the debugger.

# Gather precompiled dependencies
set(USD_BASE_URL
  "https://github.com/panthuncia/OpenUSDArtifacts/releases/download/25.05.01"
)
set(USD_DBG_ZIP  "${CMAKE_SOURCE_DIR}/ThirdParty/OpenUSD_dbg.zip")
set(USD_REL_ZIP  "${CMAKE_SOURCE_DIR}/ThirdParty/OpenUSD_rel.zip")
set(USD_DBG_DIR  "${CMAKE_SOURCE_DIR}/ThirdParty/usd_dbg")
set(USD_REL_DIR  "${CMAKE_SOURCE_DIR}/ThirdParty/usd_rel")
set(BASICRENDERER_USD_VARIANT "" CACHE STRING "USD package variant to use: dbg or rel. Leave empty for automatic selection.")

# Download if the folder isn't already there
if(NOT EXISTS "${USD_DBG_DIR}")
  file(DOWNLOAD
    "${USD_BASE_URL}/OpenUSD_dbg.zip"
    "${USD_DBG_ZIP}"
    SHOW_PROGRESS
  )
  file(ARCHIVE_EXTRACT
    INPUT  "${USD_DBG_ZIP}"
    DESTINATION "${USD_DBG_DIR}"
  )
  file(REMOVE "${USD_DBG_ZIP}")
endif()

if(NOT EXISTS "${USD_REL_DIR}")
  file(DOWNLOAD
    "${USD_BASE_URL}/OpenUSD_rel.zip"
    "${USD_REL_ZIP}"
    SHOW_PROGRESS
  )
  file(ARCHIVE_EXTRACT
    INPUT  "${USD_REL_ZIP}"
    DESTINATION "${USD_REL_DIR}"
  )
  file(REMOVE "${USD_REL_ZIP}")
endif()

# Include sub-projects.
if(NOT BASICRENDERER_USD_VARIANT)
  if(CMAKE_CONFIGURATION_TYPES)
    set(BASICRENDERER_USD_VARIANT "rel")
  elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(BASICRENDERER_USD_VARIANT "dbg")
  else()
    set(BASICRENDERER_USD_VARIANT "rel")
  endif()
endif()

if(NOT BASICRENDERER_USD_VARIANT STREQUAL "dbg" AND NOT BASICRENDERER_USD_VARIANT STREQUAL "rel")
  message(FATAL_ERROR "BASICRENDERER_USD_VARIANT must be 'dbg' or 'rel'.")
endif()

set(_usd_selected_dir "${CMAKE_SOURCE_DIR}/ThirdParty/usd_${BASICRENDERER_USD_VARIANT}")
list(APPEND CMAKE_PREFIX_PATH "${_usd_selected_dir}")
set(PXR_DIR "${_usd_selected_dir}/lib/cmake/pxr")

if(BASICRHI_ENABLE_RESHAPE)
  if(NOT WIN32)
    message(FATAL_ERROR "BASICRHI_ENABLE_RESHAPE currently requires Windows.")
  endif()

  set(ENABLE_BACKEND_DX12 ON CACHE BOOL "" FORCE)
  set(ENABLE_BACKEND_VULKAN OFF CACHE BOOL "" FORCE)
  set(ENABLE_UIX OFF CACHE BOOL "" FORCE)
  set(ENABLE_NUGET_RESTORE OFF CACHE BOOL "" FORCE)
  set(ENABLE_X86_BOOTSTRAPPER OFF CACHE BOOL "" FORCE)
  set(ENABLE_UNIT_TESTS OFF CACHE BOOL "" FORCE)
  set(GPU_RESHAPE_DISABLE_VISUAL_STUDIO_COMMAND_WRAPPERS ON CACHE BOOL "" FORCE)

  add_subdirectory("GPU-Reshape" EXCLUDE_FROM_ALL)
endif()

add_subdirectory("ThirdParty")
add_subdirectory("BasicRHI")
add_subdirectory("OpenRenderGraph")
add_subdirectory("BasicScene")
add_subdirectory ("BasicRenderer")
add_subdirectory("Plugins/ProceduralWind")

add_subdirectory("CLodCacheTool")
if(BASICRENDERER_BUILD_BRNIFLY)
  add_subdirectory("BRNifly")
endif()
add_subdirectory("UsdPlugins")
