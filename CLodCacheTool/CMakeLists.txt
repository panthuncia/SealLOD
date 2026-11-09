# CLodCacheTool: builds complete ClusterLOD caches from the command line, with no GPU.
#
# It links the renderer library because the USD whole-asset importer lives there and works on its runtime
# types, but it never creates a device. d3d12 and dxgi are delay-loaded, so they only enter the process if
# something calls them, and the tool fails at exit if they did. That is what keeps this true: see main.cpp.
#
# Edits to the generator (CLodCore) relink this tool without rebuilding CLodCapture or CLodEval.

cmake_minimum_required(VERSION 3.16)

get_filename_component(CLOD_CACHE_TOOL_BR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../BasicRenderer" ABSOLUTE)
set(CLOD_CACHE_TOOL_RUNTIME_ROOT "${CMAKE_BINARY_DIR}/BasicRenderer")

# The renderer has no Assimp importer, so this format stays here.
find_package(assimp REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)
add_executable(CLodCacheTool
    "main.cpp"
    "GltfSceneBuild.cpp"
    "GltfSceneBuild.h"
    "${CLOD_CACHE_TOOL_BR_ROOT}/src/Assets/Import/Assimp/AssimpGeometryExtractor.cpp"
)
target_link_libraries(CLodCacheTool PRIVATE
    BasicRenderer
    ${BASICRENDERER_SPDLOG_TARGET}
    Pix::headers
    "${CLOD_CACHE_TOOL_BR_ROOT}/WinPixEventRuntime.lib"
    assimp::assimp
    delayimp
)
target_link_options(CLodCacheTool PRIVATE "LINKER:/DELAYLOAD:d3d12.dll" "LINKER:/DELAYLOAD:dxgi.dll")
target_include_directories(CLodCacheTool PRIVATE "${CLOD_CACHE_TOOL_BR_ROOT}/src")
set_target_properties(CLodCacheTool PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    # Beside the renderer, so it finds the same USD, TBB and Assimp DLLs.
    RUNTIME_OUTPUT_DIRECTORY "${CLOD_CACHE_TOOL_RUNTIME_ROOT}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CLOD_CACHE_TOOL_RUNTIME_ROOT}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CLOD_CACHE_TOOL_RUNTIME_ROOT}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CLOD_CACHE_TOOL_RUNTIME_ROOT}"
)
if(MSVC)
    target_compile_options(CLodCacheTool PRIVATE /Zc:preprocessor /EHsc /bigobj)
endif()
add_dependencies(CLodCacheTool tbb_config_sync)
target_compile_definitions(CLodCacheTool PRIVATE NOMINMAX _UNICODE UNICODE)

# The CRT heap serializes allocation across threads, and the builder allocates per group on every worker: with all
# cores building, most workers were blocked in its lock. AllocatorProxy.cpp replaces this program's operator new and
# delete with the TBB scalable allocator (tbbmalloc, from the USD bundle that also supplies tbb.dll). tbbmalloc_proxy
# is not used: it declines current UCRTs and silently leaves the CRT heap in place.
option(CLOD_CACHE_TOOL_SCALABLE_ALLOCATOR "Route CLodCacheTool's operator new/delete to tbbmalloc" ON)
set(_clod_tool_tbbmalloc_root "${BASICRENDERER_USD_ROOT}/usd_rel")
if(CLOD_CACHE_TOOL_SCALABLE_ALLOCATOR AND EXISTS "${_clod_tool_tbbmalloc_root}/lib/tbbmalloc.lib")
    if(NOT TARGET CLodCacheTool::tbbmalloc)
        add_library(CLodCacheTool::tbbmalloc SHARED IMPORTED)
        set_target_properties(CLodCacheTool::tbbmalloc PROPERTIES
            IMPORTED_IMPLIB "${_clod_tool_tbbmalloc_root}/lib/tbbmalloc.lib"
            IMPORTED_LOCATION "${_clod_tool_tbbmalloc_root}/bin/tbbmalloc.dll")
    endif()
    target_sources(CLodCacheTool PRIVATE "AllocatorProxy.cpp")
    target_link_libraries(CLodCacheTool PRIVATE CLodCacheTool::tbbmalloc)
    add_custom_command(TARGET CLodCacheTool POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_clod_tool_tbbmalloc_root}/bin/tbbmalloc.dll" "$<TARGET_FILE_DIR:CLodCacheTool>"
        VERBATIM)
elseif(CLOD_CACHE_TOOL_SCALABLE_ALLOCATOR)
    message(STATUS "CLodCacheTool: ${_clod_tool_tbbmalloc_root}/lib/tbbmalloc.lib not found; keeping the CRT heap")
endif()
