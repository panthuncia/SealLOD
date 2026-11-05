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
add_executable(CLodCacheTool
    "main.cpp"
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
